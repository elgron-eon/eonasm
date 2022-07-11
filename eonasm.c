/*
 * eonasm.c
 *
 * eon classical assembler
 * (c) JCGV, junio del 2022
 *
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>

/*
 * config
 */
#define VERSION 	    "0.0.0"

#define MAX_LINE	    128     // max chars per lines
#define MAX_ERRORS	    8	    // error count abort
#define MAX_LABELS	    256     // label table size
#define MAX_CHAR_LABEL	    22	    // significative label chars
#define OUTPUT_LINE_BYTES   32	    // bytes per line in intel hex output

/*
 * ctype support
 */
static int isdigit (int c) {return (unsigned) c - '0'  < 10;}
static int isalpha (int c) {return ((unsigned) c | 32) - 'a' < 26;}
static int isalnum (int c) {return isalpha (c) || isdigit (c);}

static int islower (int c) {return (unsigned) c - 'a'  < 26;}
static int toupper (int c) {return islower (c) ? c & 0x5f : c;}

/*
 * format engine
 */
static char fmtline[MAX_LINE * 2];
static const char hdigit[] = "0123456789ABCDEF";

static const char * fmt (const char *s, ...) {
    va_list va;
    va_start (va, s);
    char *p = fmtline;
    while (*s) {
	int c = *s++;
	if (c == '%' && *s) {
	    c = *s++;
	    switch (c) {
		case 'b': { // hex byte 2 digits
			unsigned n = va_arg (va, unsigned);
			*p++ = hdigit[(n >>  4) & 0x0f];
			*p++ = hdigit[(n >>  0) & 0x0f];
		    } break;
		case 'w': { // hex word 4 digits
			unsigned n = va_arg (va, unsigned);
			*p++ = hdigit[(n >> 12) & 0x0f];
			*p++ = hdigit[(n >>  8) & 0x0f];
			*p++ = hdigit[(n >>  4) & 0x0f];
			*p++ = hdigit[(n >>  0) & 0x0f];
		    } break;
		case '5': { // unsigned formatted to 5 digits
			unsigned  n = va_arg (va, unsigned);
			char out[6] = {' ', ' ', ' ', ' ', '0', 0};
			char	*d  = &out[5];
			while (n > 0) {
			    *--d = n % 10 + '0';
			    n	/= 10;
			}
			for (int i = 0; i < 5; i++)
			    *p++ = out[i];
		    } break;
		case 's': { // string null terminated
			const char *z = va_arg (va, const char *);
			while (*z)
			    *p++ = *z++;
		    } break;
		case 'm': { // errno msg
			const char *z = strerror (errno);
			while (*z)
			    *p++ = *z++;
		    } break;
		default: *p++ = c; break;
	    }
	} else
	    *p++ = c;
    }

    // done
    *p = 0;
    va_end (va);
    return fmtline;
}

/*
 * output engine
 */
static int ofd;

static void _print (int fd, int l, const char *s) {
    if (l < 0) l = strlen (s);

    int done = 0;
    while (done < l) {
	int rc = write (fd, s + done, l - done);
	if (rc <= 0) {
	    static const char ioerr[] = "eonasm: I/O error in print\n";
	    write (STDERR_FILENO, ioerr, sizeof (ioerr) - 1);
	    exit  (1);
	}
	done += rc;
    }
}

#define eprint(l,s) _print (STDERR_FILENO, l, s)
#define oprint(l,s) _print (STDOUT_FILENO, l, s)
#define iprint(l,s) _print (ofd, l, s)

static void output_to (const char *path) {
    ofd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) {
	eprint (-1, fmt ("eonasm: can not create output file [%s]: %m\n", path));
	exit   (1);
    }
}

/*
 * globals
 */
static unsigned errcount;
const char * source;

static void error (unsigned lineno, const char *msg) {
    eprint (-1, fmt ("eonasm error at line %5 of %s: %s\n", lineno, source, msg));
    errcount++;
    if (errcount >= MAX_ERRORS) exit (1);
}

/*
 * input engine
 */
static bool readline (int fd, uint8_t *buf, unsigned bytes, int lineno) {
    uint8_t *b = buf;
    uint8_t *e = b + bytes;
    while (b < e) {
	int l = read (fd, b, 1);
	if (l < 0) {
	    eprint (-1, fmt ("eonasm: error reading [%s]: %m\n", source));
	    exit   (1);
	}
	b += l;
	if (l == 0 || b[-1] == '\n')
	    break;
    }
    if (b >= e) {
	eprint (-1, fmt ("eonasm: line %5 of [%s] is too long\n", lineno, source));
	exit   (1);
    }
    *b++ = 0;
    return b != buf + 1;
}

/*
 * output image
 */
static uint8_t	line[OUTPUT_LINE_BYTES];
static unsigned pending;
static unsigned basepc;
static unsigned outpc;

static void emit_flush (void) {
    if (pending) {
	iprint (-1, fmt (":%b%w00", pending, basepc));
	uint8_t crc = pending + (basepc >> 8) + basepc;
	for (unsigned i = 0; i < pending; i++) {
	    uint8_t byte = line[i];
	    crc 	+= byte;
	    iprint (-1, fmt ("%b", byte));
	}
	iprint (-1, fmt ("%b\n", (0 - crc) & 0x0ff));
	pending = 0;
    }
}

static void emit (uint16_t at, uint8_t byte) {
    if (pending >= OUTPUT_LINE_BYTES || at != outpc) {
	emit_flush ();
	outpc = basepc = at;
    }
    line[pending++] = byte;
    outpc++;
}

static void emit_done (void) {
    emit_flush ();
    iprint     (-1, ":00000001FF\n");
}

/*
 * labels
 */
typedef struct label_t * label_t;
struct label_t {
    uint32_t	value;	    // value
    uint16_t	lbegin;     // local stack index
    uint16_t	lend;	    // local labels
    uint8_t	flags;
    uint8_t	len;
    char	name[MAX_CHAR_LABEL];
};

#define LABEL_USED  0x01
#define LABEL_EQU   0x02

static unsigned       nlabel;
static unsigned       lstack = MAX_LABELS;
static struct label_t tlabel[MAX_LABELS];

static label_t find_label (label_t master, const char *id, unsigned len) {
    if (len > MAX_CHAR_LABEL) len = MAX_CHAR_LABEL;

    // search
    unsigned i	 = 0;
    unsigned end = nlabel;
    if (master) {
	i = master->lbegin;
	end = master->lend;
    }

    for (; i < end; ++i) {
	label_t l = &tlabel[i];
	if (len == l->len && !memcmp (id, l->name, len))
	    return l;
    }
    return NULL;
}

static label_t add_label (label_t master, const char *id, unsigned len, unsigned at) {
    // check for space
    if (nlabel >= lstack) {
	eprint (-1, fmt ("eonasm: too many labels (> %5) %5 global %5 local\n", MAX_LABELS, nlabel, MAX_LABELS - lstack));
	exit   (1);
    }

    // register label
    label_t l = NULL;
    if (master) {
	l = &tlabel[--lstack];
	master->lbegin = lstack;
    } else {
	l = &tlabel[nlabel++];
	l->lbegin = l->lend = lstack;
    }

    // init
    l->value = at;
    l->len   = len > MAX_CHAR_LABEL ? MAX_CHAR_LABEL : len;

    // setup name
    memcpy (l->name, id, len);

    // done
    //eprint (-1, fmt ("label %s %5 [%s] = %w\n", master ? "local" : "global", l->len, l->name, l->value));
    return l;
}

/*
 * expr parser
 */
typedef struct {uint8_t *p; unsigned v;} vp_t;

static vp_t expr (unsigned lineno, label_t mainlbl, bool allow_undef, unsigned pc, uint8_t *p) {
    uint32_t sval[8];
    uint32_t sop[8];
    uint32_t vsp = 0;
    uint32_t osp = 0;
    uint32_t max = 8;
    for (;;) {
	// skip spaces
	while (*p && *p <= ' ')
	    ++p;

	// get element
	uint32_t  v = 0;
	uint32_t op = 0;

	// parse item
	if (*p == '(') {
	    vp_t vp = expr (lineno, mainlbl, allow_undef, pc, p + 1);
	    p	    = vp.p;
	    v	    = vp.v;
	    if (!p || *p++ != ')') break;
	} else if (*p == '$') {
	    p++;
	    if (*p == '$') {
		v = pc;
		p++;
	    } else {
		// hex number
		for (;; p++) {
		    unsigned c = *p;
		    int      d = c >= '0' && c <= '9' ? c - '0'
			       : c >= 'a' && c <= 'f' ? c - 'a' + 10
			       : c >= 'A' && c <= 'F' ? c - 'A' + 10
			       : -1
			       ;
		    if (d < 0) break;
		    v = (v << 4) | d;
		}
	    }
	} else if (isdigit (*p) || (*p == '-' && isdigit (p[1]))) {
	    bool minus = false; if (*p == '-') {minus = true; p++;}
	    while (isdigit (*p))
		v = v * 10 + *p++ - '0';
	    if (minus) v = 0 - v;
	} else if (*p == '\'' && p[2] == '\'') {
	    v	  = p[1];
	    p	 += 3;
	} else if (*p == '+' || *p == '-' || *p == '&' || *p == '|' || *p == '*' || *p == '%' || *p == '/')
	    op = *p++;
	else if (*p == ':' || isalpha (*p) || *p == '.') {
	    if (*p == ':') p++;
	    bool local = false; if (*p == '.') {local = true; p++;}
	    if (local && !mainlbl)
		error (lineno, "local label in expr without main label");

	    static char name[MAX_LINE];
	    char *n = name;
	    for (; *p == '_' || isalnum (*p); p++)
		*n++ = toupper (*p);

	    label_t lbl = find_label (local ? mainlbl : NULL, name, n - name);
	    if (!lbl) {
		if (!allow_undef) {
		    //*n = 0; eprint (-1, fmt ("undefined [%s]\n", name));
		    error (lineno, "undefined label in expr");
		}
	    } else {
		lbl->flags |= LABEL_USED;
		v	    = lbl->value;
	    }
	}
	else
	    break;

	// process
	if (op) {
	    if (osp + 1 != vsp || osp >= max)
		break;
	    sop[osp++] = op;
	} else {
	    if (vsp != osp || vsp >= max)
		break;
	    sval[vsp++] = v;
	}
    }

    // done
    if (osp + 1 != vsp) {
	error (lineno, "expr syntax");
	p = NULL;
    } else {
	// reduce
	while (osp > 0) {
	    uint32_t op = sop[--osp];
	    uint32_t vr = sval[--vsp];
	    uint32_t vl = sval[--vsp];
	    uint32_t vv = 0;
	    switch (op) {
		case '+': vv = vl + vr; break;
		case '-': vv = vl - vr; break;
		case '*': vv = vl * vr; break;
		case '/': vv = vl / vr; break;
		case '%': vv = vl % vr; break;
		case '&': vv = vl & vr; break;
		case '|': vv = vl | vr; break;
		default : break;
	    }
	    sval[vsp++] = vv;
	}
    }
    return (vp_t) {p, sval[0]};
}

/*
 * registers
 */
static struct {
    const char *id;
    int 	rno;
} vreg[] = {
    {"R0",	    0},
    {"R1",	    1},
    {"R10",	   10},
    {"R11",	   11},
    {"R12",	   12},
    {"R13",	   13},
    {"R14",	   14},
    {"R2",	    2},
    {"R3",	    3},
    {"R4",	    4},
    {"R5",	    5},
    {"R6",	    6},
    {"R7",	    7},
    {"R8",	    8},
    {"R9",	    9},
    {"SP",	   15},
};

static int reg_find (const char *reg) {
    int a = 0;
    int b = sizeof (vreg) / sizeof (vreg[0]) - 1;
    while (a <= b) {
	int m = (a + b) / 2;
	int r = strcmp (reg, vreg[m].id);
	if (r == 0) return vreg[m].rno;
	if (r < 0)
	    b = m - 1;
	else
	    a = m + 1;
    }
    return -1;
}

/*
 * opcodes
 */
enum {
    OP_ADD, OP_AND,
    OP_BEQ, OP_BLE, OP_BLEI, OP_BLT, OP_BLTI,
    OP_BNE, OP_BNZ, OP_BRA, OP_BSWAP, OP_BZ,
    OP_CSETN, OP_CSETNN, OP_CSETNP, OP_CSETNZ, OP_CSETP, OP_CSETZ,
    OP_ENTER, OP_ERET, OP_GET, OP_ILL, OP_IN, OP_IRET, OP_ISTAT, OP_JAL, OP_JMP,
    OP_LD1, OP_LD1I, OP_LD2, OP_LD2I, OP_LD4, OP_LD4I, OP_LD8,
    OP_LEA, OP_LI,  OP_MV, OP_NOP, OP_OR, OP_OUT, OP_RET, OP_SET,
    OP_SEXT1, OP_SEXT2, OP_SEXT4,
    OP_SHL, OP_SHR, OP_SHRI, OP_SIGNAL, OP_SRET,
    OP_ST1, OP_ST2, OP_ST4, OP_ST8,
    OP_SUB, OP_SYS, OP_WAIT, OP_XOR,
    OP_ZEXT1, OP_ZEXT2, OP_ZEXT4
};

static struct {
    const char *id;
    int 	op;
} vop[] = {
    {"ADD",	    OP_ADD	},
    {"AND",	    OP_AND	},
    {"BEQ",	    OP_BEQ	},
    {"BLE",	    OP_BLE	},
    {"BLEI",	    OP_BLEI	},
    {"BLT",	    OP_BLT	},
    {"BLTI",	    OP_BLTI	},
    {"BNE",	    OP_BNE	},
    {"BNZ",	    OP_BNZ	},
    {"BRA",	    OP_BRA	},
    {"BSWAP",	    OP_BSWAP	},
    {"BZ",	    OP_BZ	},
    {"CSETN",	    OP_CSETN	},
    {"CSETNN",	    OP_CSETNN	},
    {"CSETNP",	    OP_CSETNP	},
    {"CSETNZ",	    OP_CSETNZ	},
    {"CSETP",	    OP_CSETP	},
    {"CSETZ",	    OP_CSETZ	},
    {"ENTER",	    OP_ENTER	},
    {"ERET",	    OP_ERET	},
    {"GET",	    OP_GET	},
    {"ILLEGAL",     OP_ILL	},
    {"IN",	    OP_IN	},
    {"IRET",	    OP_IRET	},
    {"ISTAT",	    OP_ISTAT	},
    {"JAL",	    OP_JAL	},
    {"JMP",	    OP_JMP	},
    {"LD1",	    OP_LD1	},
    {"LD1I",	    OP_LD1I	},
    {"LD2",	    OP_LD2	},
    {"LD2I",	    OP_LD2I	},
    {"LD4",	    OP_LD4	},
    {"LD4I",	    OP_LD4I	},
    {"LD8",	    OP_LD8	},
    {"LEA",	    OP_LEA	},
    {"LI",	    OP_LI	},
    {"MV",	    OP_MV	},
    {"NOP",	    OP_NOP	},
    {"OR",	    OP_OR	},
    {"OUT",	    OP_OUT	},
    {"RET",	    OP_RET	},
    {"SET",	    OP_SET	},
    {"SEXT1",	    OP_SEXT1	},
    {"SEXT2",	    OP_SEXT2	},
    {"SEXT4",	    OP_SEXT4	},
    {"SHL",	    OP_SHL	},
    {"SHR",	    OP_SHR	},
    {"SHRI",	    OP_SHRI	},
    {"SIGNAL",	    OP_SIGNAL	},
    {"SRET",	    OP_SRET	},
    {"ST1",	    OP_ST1	},
    {"ST2",	    OP_ST2	},
    {"ST4",	    OP_ST4	},
    {"ST8",	    OP_ST8	},
    {"SUB",	    OP_SUB	},
    {"SYSCALL",     OP_SYS	},
    {"WAIT",	    OP_WAIT	},
    {"XOR",	    OP_XOR	},
    {"ZEXT1",	    OP_ZEXT1	},
    {"ZEXT2",	    OP_ZEXT2	},
    {"ZEXT4",	    OP_ZEXT4	},
};

static int op_find (const char *op) {
    int a = 0;
    int b = sizeof (vop) / sizeof (vop[0]) - 1;
    while (a <= b) {
	int m = (a + b) / 2;
	int r = strcmp (op, vop[m].id);
	if (r == 0) return vop[m].op;
	if (r < 0)
	    b = m - 1;
	else
	    a = m + 1;
    }
    return -1;
}

/*
 * opcode match engine
 */
typedef struct arg_t * arg_t;
struct arg_t {
    enum {_, R, N, M} k;
    int     rno;
    int     val;
};

typedef struct tentry_t * tentry_t;
struct tentry_t {
    uint8_t	op;
    uint8_t	na;
    uint8_t	args[3];
    uint8_t	kind;
    uint16_t	word;
};

static struct tentry_t tmatch[] = {
    {OP_ADD,	3,  {R, R, R},	    'R', 0x4000 },
    {OP_ADD,	3,  {R, R, N},	    'A', 0x3004 },
    {OP_ADD,	2,  {R, N, _},	    'a', 0x3004 },
    {OP_ADD,	2,  {R, R, _},	    'r', 0x4000 },
    {OP_AND,	3,  {R, R, R},	    'R', 0x8000 },
    {OP_AND,	3,  {R, R, N},	    'A', 0x3008 },
    {OP_AND,	2,  {R, N, _},	    'a', 0x3008 },
    {OP_AND,	2,  {R, R, _},	    'r', 0x8000 },
    {OP_BEQ,	3,  {R, R, N},	    'b', 0x2000 },
    {OP_BLE,	3,  {R, R, N},	    'b', 0x2004 },
    {OP_BLEI,	3,  {R, R, N},	    'b', 0x2005 },
    {OP_BLT,	3,  {R, R, N},	    'b', 0x2002 },
    {OP_BLTI,	3,  {R, R, N},	    'b', 0x2003 },
    {OP_BNE,	3,  {R, R, N},	    'b', 0x2001 },
    {OP_BNZ,	2,  {R, N, _},	    '!', 0x20f1 },
    {OP_BRA,	1,  {N, _, _},	    'B', 0x2ff0 },
    {OP_BSWAP,	2,  {R, R, _},	    'U', 0x0004 },
    {OP_BSWAP,	1,  {R, _, _},	    'u', 0x0004 },
    {OP_BZ,	2,  {R, N, _},	    '!', 0x20f0 },
    {OP_CSETN,	2,  {R, R, _},	    'U', 0x000a },
    {OP_CSETN,	1,  {R, _, _},	    'u', 0x000a },
    {OP_CSETNN, 2,  {R, R, _},	    'U', 0x000b },
    {OP_CSETNN, 1,  {R, _, _},	    'u', 0x000b },
    {OP_CSETNP, 2,  {R, R, _},	    'U', 0x000d },
    {OP_CSETNP, 1,  {R, _, _},	    'u', 0x000d },
    {OP_CSETNZ, 2,  {R, R, _},	    'U', 0x0009 },
    {OP_CSETNZ, 1,  {R, _, _},	    'u', 0x0009 },
    {OP_CSETP,	2,  {R, R, _},	    'U', 0x000c },
    {OP_CSETP,	1,  {R, _, _},	    'u', 0x000c },
    {OP_CSETZ,	2,  {R, R, _},	    'U', 0x0008 },
    {OP_CSETZ,	1,  {R, _, _},	    'u', 0x0008 },
    {OP_ENTER,	1,  {N, _, _},	    'E', 0x0ff8 },
    {OP_ERET,	0,  {_, _, _},	    'N', 0x0ff6 },
    {OP_GET,	2,  {R, N, _},	    'G', 0x0f08 },
    {OP_ILL,	0,  {_, _, _},	    'N', 0x0ff0 },
    {OP_IN,	2,  {R, R, _},	    'U', 0x000e },
    {OP_IRET,	0,  {_, _, _},	    'N', 0x0ff4 },
    {OP_ISTAT,	1,  {R, _, _},	    '1', 0x0f04 },
    {OP_JAL,	1,  {N, _, _},	    'J', 0x0ffd },
    {OP_JAL,	1,  {R, _, _},	    '1', 0x0f01 },
    {OP_JMP,	1,  {N, _, _},	    'J', 0x0ffc },
    {OP_JMP,	1,  {R, _, _},	    '1', 0x0f00 },
    {OP_LD1,	2,  {R, M, _},	    'M', 0x1000 },
    {OP_LD1I,	2,  {R, M, _},	    'M', 0x1001 },
    {OP_LD2,	2,  {R, M, _},	    'M', 0x1002 },
    {OP_LD2I,	2,  {R, M, _},	    'M', 0x1003 },
    {OP_LD4,	2,  {R, M, _},	    'M', 0x1004 },
    {OP_LD4I,	2,  {R, M, _},	    'M', 0x1005 },
    {OP_LD8,	2,  {R, M, _},	    'M', 0x1006 },
    {OP_LEA,	2,  {R, N, _},	    'L', 0x0f0d },
    {OP_LEA,	2,  {R, M, _},	    'l', 0x0f0a },
    {OP_LI,	2,  {R, N, _},	    'I', 0x0f0c },
    {OP_MV,	2,  {R, R, _},	    '=', 0x90f0 },
    {OP_NOP,	0,  {_, _, _},	    'N', 0x0ff1 },
    {OP_OR,	3,  {R, R, R},	    'R', 0x9000 },
    {OP_OR,	3,  {R, R, N},	    'A', 0x3009 },
    {OP_OR,	2,  {R, N, _},	    'a', 0x3009 },
    {OP_OR,	2,  {R, R, _},	    'r', 0x9000 },
    {OP_OUT,	2,  {R, R, _},	    'U', 0x000f },
    {OP_RET,	0,  {_, _, _},	    'N', 0x0fe0 },
    {OP_SET,	2,  {N, R, _},	    'g', 0x0f09 },
    {OP_SEXT1,	2,  {R, R, _},	    'U', 0x0005 },
    {OP_SEXT1,	1,  {R, _, _},	    'u', 0x0005 },
    {OP_SEXT2,	2,  {R, R, _},	    'U', 0x0006 },
    {OP_SEXT2,	1,  {R, _, _},	    'u', 0x0006 },
    {OP_SEXT4,	2,  {R, R, _},	    'U', 0x0007 },
    {OP_SEXT4,	1,  {R, _, _},	    'u', 0x0007 },
    {OP_SHL,	3,  {R, R, R},	    'R', 0xb000 },
    {OP_SHL,	3,  {R, R, N},	    'A', 0x300b },
    {OP_SHL,	2,  {R, N, _},	    'a', 0x300b },
    {OP_SHL,	2,  {R, R, _},	    'r', 0xb000 },
    {OP_SHR,	3,  {R, R, R},	    'R', 0xc000 },
    {OP_SHR,	3,  {R, R, N},	    'A', 0x300c },
    {OP_SHR,	2,  {R, N, _},	    'a', 0x300c },
    {OP_SHR,	2,  {R, R, _},	    'r', 0xc000 },
    {OP_SHRI,	3,  {R, R, R},	    'R', 0xd000 },
    {OP_SHRI,	3,  {R, R, N},	    'A', 0x300d },
    {OP_SHRI,	2,  {R, N, _},	    'a', 0x300d },
    {OP_SHRI,	2,  {R, R, _},	    'r', 0xd000 },
    {OP_SIGNAL, 1,  {N, _, _},	    'E', 0x0ff9 },
    {OP_SRET,	0,  {_, _, _},	    'N', 0x0ff5 },
    {OP_ST1,	2,  {M, R, _},	    'm', 0x1008 },
    {OP_ST2,	2,  {M, R, _},	    'm', 0x1009 },
    {OP_ST4,	2,  {M, R, _},	    'm', 0x100a },
    {OP_ST8,	2,  {M, R, _},	    'm', 0x100b },
    {OP_SUB,	3,  {R, R, R},	    'R', 0x5000 },
    {OP_SUB,	3,  {R, R, N},	    'A', 0x3005 },
    {OP_SUB,	2,  {R, N, _},	    'a', 0x3005 },
    {OP_SUB,	2,  {R, R, _},	    'r', 0x5000 },
    {OP_SYS,	0,  {_, _, _},	    'N', 0x0ff2 },
    {OP_WAIT,	0,  {_, _, _},	    'N', 0x0ff3 },
    {OP_XOR,	3,  {R, R, R},	    'R', 0xa000 },
    {OP_XOR,	3,  {R, R, N},	    'A', 0x300a },
    {OP_XOR,	2,  {R, N, _},	    'a', 0x300a },
    {OP_XOR,	2,  {R, R, _},	    'r', 0xa000 },
    {OP_ZEXT1,	2,  {R, R, _},	    'U', 0x0001 },
    {OP_ZEXT1,	1,  {R, _, _},	    'u', 0x0001 },
    {OP_ZEXT2,	2,  {R, R, _},	    'U', 0x0002 },
    {OP_ZEXT2,	1,  {R, _, _},	    'u', 0x0002 },
    {OP_ZEXT4,	2,  {R, R, _},	    'U', 0x0003 },
    {OP_ZEXT4,	1,  {R, _, _},	    'u', 0x0003 },
};

static tentry_t match (int op, int na, arg_t va) {
    for (unsigned i = 0; i < sizeof (tmatch) / sizeof (tmatch[0]); i++)
	if (tmatch[i].op == op && tmatch[i].na == na) {
	    tentry_t e = &tmatch[i];
	    for (int n = 0; n < na; n++)
		if (va[n].k != e->args[n]) {
		    e = NULL;
		    break;
		}
	    if (e) return e;
	}
    return NULL;
}

/*
 * two pass assembler
 */
static unsigned assemble (int fd, unsigned pass, bool out, unsigned pc, bool listing, bool *pmore) {
    static char     tmp[MAX_LINE];
    static uint8_t  buffer[MAX_LINE];
    static uint8_t  code[MAX_LINE];
    uint32_t lineno = 0;
    label_t mainlbl = NULL;
    while (readline (fd, buffer, sizeof (buffer), ++lineno)) {
	uint8_t *p = buffer;

	// line bytes
	unsigned bytes = 0;
	unsigned space = 0;
	bool	   org = false;
	bool	   equ = false;

	// optional label
	label_t lbl = NULL;
	if (isalpha (*p) || *p == '.') {
	    bool local = false; if (*p == '.') {p++; local = true;}
	    char   *id = tmp;
	    for (; isalnum (*p) || *p == '_'; p++)
		*id++ = toupper (*p);

	    // check local & mainlbl
	    if (local && !mainlbl)
		error (lineno, "local label without main label");

	    // register/find label
	    lbl = find_label (local ? mainlbl : NULL, tmp, id - tmp);
	    if (lbl) {
		if (pass == 0)
		    error (lineno, "duplicated label");
		else if ((lbl->flags & LABEL_EQU) == 0 && lbl->value != pc) {
		    *pmore     = true;
		    lbl->value = pc;
		    //eprint (-1, fmt ("label previous value %w now %w\n", lbl->value, pc));
		    //error (lineno, "label value changed on pass 2");
		}
	    } else {
		*pmore = true;
		lbl    = add_label (local ? mainlbl : NULL, tmp, id - tmp, pc);
		if (out)
		    error (lineno, "undefined label on last pass !");
	    }

	    // set main label
	    if (!local) mainlbl = lbl;

	    // optional ':'
	    if (*p == ':') p++;
	}

	// skip spaces
	while (*p && *p <= ' ') p++;

	// body
	if (*p == '.') {
	    // directive
	    uint8_t *id = (uint8_t *) tmp;
	    for (++p; isalpha (*p);)
		*id++ = toupper (*p++);
	    *id = 0;

	    // skip blanks
	    while (*p && *p <= ' ') p++;

	    // process
	    if (!strcmp (tmp, "ORG")) {
		vp_t vp = expr (lineno, mainlbl, false, pc, p);
		p	= vp.p; if (!p) continue;
		bytes	= vp.v - pc;
		org	= true;
	    } else if (!strcmp (tmp, "EQU")) {
		vp_t vp = expr (lineno, mainlbl, false, pc, p);
		p	= vp.p; if (!p) continue;
		if (lbl) {
		    lbl->value	= vp.v;
		    lbl->flags |= LABEL_USED | LABEL_EQU;
		    equ 	= true;
		} else {
		    error (lineno, ".EQU without label");
		    continue;
		}
	    } else if (!strcmp (tmp, "ZERO")) {
		vp_t vp = expr (lineno, mainlbl, false, pc, p);
		p	= vp.p; if (!p) continue;
		if (vp.v > sizeof (code)) {
		    error (lineno, ".ZERO size overflow");
		    continue;
		}
		for (unsigned n = 0; n < vp.v; n++)
		    code[bytes++] = 0;
	    } else if (!strcmp (tmp, "SPACE")) {
		vp_t vp = expr (lineno, mainlbl, false, pc, p);
		p	= vp.p; if (!p) continue;
		space	= bytes = vp.v;
	    } else if (!strcmp (tmp, "BYTE")) {
		for (;;) {
		    // skip blanks
		    while (*p && *p <= ' ') p++;

		    // get arg
		    if (*p == '"') {
			for (++p; *p && *p != '"';)
			    code[bytes++] = *p++;
			if (*p++ != '"') {
			    error (lineno, "incomplete string");
			    goto next;
			}
			while (*p && *p <= ' ') p++;
		    } else {
			vp_t vp = expr (lineno, mainlbl, !out, pc, p);
			p	= vp.p; if (!p) goto next;

			code[bytes++] = vp.v;
			if (out && vp.v > 255) error (lineno, ".BYTE overflow");
		    }

		    if (*p != ',') break;
		    ++p;
		}
	    } else if (!strcmp (tmp, "WORD")) {
		for (;;) {
		    vp_t vp = expr (lineno, mainlbl, !out, pc, p);
		    p	    = vp.p; if (!p) goto next;

		    code[bytes++] = vp.v >> 8;
		    code[bytes++] = vp.v;
		    if (out && vp.v > 65536) error (lineno, ".WORD overflow");

		    if (*p != ',') break;
		    ++p;
		}
	    } else {
		//printf ("%s\t%s", tmp, buffer);
		error (lineno, "unknown directive");
		continue;
	    }
	} else if (isalpha (*p)) {
	    // opcode
	    uint8_t *id = (uint8_t *) tmp;
	    while (isalnum (*p))
		*id++ = toupper (*p++);
	    *id++ = 0;

	    // find opcode
	    int op = op_find (tmp);
	    if (op < 0) {
		error (lineno, "unknown opcode");
		continue;
	    }

	    // arguments
	    struct arg_t va[3];
	    int 	 na = 0;
	    for (bool sep = false; na < 3;) {
		// skip blanks
		while (*p && *p <= ' ') p++;

		// separator
		if (*p == ',') {
		    p++;
		    if (!sep)
			error (lineno, "unexpected ','");
		    sep = false;
		    continue;
		}

		// arg ?
		if (isalpha (*p)) {
		    uint8_t *id = (uint8_t *) tmp;
		    uint8_t *pp = p;
		    while (isalnum (*p))
			*id++ = toupper (*p++);
		    *id++ = 0;

		    int rno = reg_find (tmp);
		    if (rno < 0) {
			vp_t	vp = expr (lineno, mainlbl, out ? false : true, pc, pp);
			p	   = vp.p; if (!p) goto next;
			va[na].k   = N;
			va[na].val = vp.v;
		    } else {
			va[na].k   = R;
			va[na].rno = reg_find (tmp);
		    }
		} else if (*p == '[') {
		    va[na].k   = M;
		    va[na].val = 0;

		    // skip blanks
		    for (++p; *p && *p <= ' ';) p++;

		    // register
		    uint8_t *id = (uint8_t *) tmp;
		    while (isalnum (*p))
			*id++ = toupper (*p++);
		    *id++ = 0;
		    va[na].rno = reg_find (tmp);
		    if (va[na].rno < 0) error (lineno, "unknown register");

		    // skip blanks
		    while (*p && *p <= ' ') p++;

		    // optional expr
		    if (*p == '+' || *p == '-') {
			bool minus = *p == '-';
			vp_t	vp = expr (lineno, mainlbl, !out, pc, p + 1);
			p	   = vp.p; if (!p) goto next;
			va[na].val = minus ? 0 - vp.v : vp.v;
		    }

		    // check final
		    if (*p++ != ']') {
			error (lineno, "memory access arg without ']'");
			goto next;
		    }
		} else if (*p == ':' || *p == '.' || *p == '$' || *p == '\'' || *p == '-' || isdigit (*p)) {
		    vp_t    vp = expr (lineno, mainlbl, !out, pc, p);
		    p	       = vp.p; if (!p) goto next;
		    va[na].k   = N;
		    va[na].val = vp.v;
		} else
		    break;

		// allow separator
		sep = true;

		// account arg
		na++;
	    }

	    // skip spaces
	    while (*p && *p <= ' ') p++;

	    // match template
	    tentry_t te = match (op, na, va);
	    if (!te) {
		error (lineno, "unknown combination of opcode and args");
		//printf ("opcode value %x template [%s]\n", v, tmp);
		continue;
	    } else {
		// emit
		int	 k = te->kind;
		unsigned w = te->word;
		again: switch (k) {
		    case 'N':	// direct opcode
			code[bytes++] = w >> 8;
			code[bytes++] = w;
			break;
		    case 'R':	// 3 regs
			code[bytes++] = (w >> 8) | va[0].rno;
			code[bytes++] = (va[1].rno << 4) | va[2].rno;
			break;
		    case 'r':	// 3 regs sugar syntax
			va[2].rno = va[1].rno;
			va[1].rno = va[0].rno;
			k	  = 'R';
			goto again;
		    case 'a':	// 2 regs + imm sugar syntax
			va[2].val = va[1].val;
			va[1].rno = va[0].rno;
			k	  = 'A';
			goto again;
		    case 'A':	// 2 regs + imm
			code[bytes++] = (w >> 8) | va[0].rno;
			code[bytes++] = (w >> 0) | (va[1].rno << 4);
			code[bytes++] = va[2].val >> 8;
			code[bytes++] = va[2].val;
			if (out && (va[2].val >= 32768 || va[2].val < -32768))
			    error (lineno, "inmediate out of range");
			break;
		    case 'U':	// unary 2 regs
			code[bytes++] = (w >> 8) | va[0].rno;
			code[bytes++] = (w >> 0) | (va[1].rno << 4);
			break;
		    case 'u':	// unary sugar syntax
			code[bytes++] = (w >> 8) | va[0].rno;
			code[bytes++] = (w >> 0) | (va[0].rno << 4);
			break;
		    case 'E':	// single imm
			va[0].rno = va[1].rno = 0;
			va[2].val = va[0].val;
			k	  = 'A';
			goto again;
		    case 'B': { // branch
			    code[bytes++] = w >> 8;
			    code[bytes++] = w >> 0;
			    int off = ((int) va[0].val - ((int) pc + 4)) / 2;
			    code[bytes++] = off >> 8;
			    code[bytes++] = off;
			    if (out && (off >= 32768 || off < -32768))
				error (lineno, "branch out of range");
			} break;
		    case 'b':	// conditional branch
			w	 |= (va[0].rno << 8) | (va[1].rno << 4);
			k	  = 'B';
			va[0].val = va[2].val;
			goto again;
		    case '!':	// conditional branch sugar syntax
			w	 |= (va[0].rno << 8);
			k	  = 'B';
			va[0].val = va[1].val;
			goto again;
		    case 'M':	// memory access
			code[bytes++] = (w >> 8) | va[0].rno;
			code[bytes++] = (w >> 0) | (va[1].rno << 4);
			code[bytes++] = va[1].val >> 8;
			code[bytes++] = va[1].val;
			if (out && (va[1].val >= 32768 || va[1].val < -32768))
			    error (lineno, "memory offset out of range");
			break;
		    case 'm':	// store memory access
			va[2].rno = va[1].rno;
			va[1].rno = va[0].rno;
			va[1].val = va[0].val;
			va[0].rno = va[2].rno;
			k	  = 'M';
			goto again;
		    case 'J': { // jmp/jal
			    code[bytes++] = w >> 8;
			    code[bytes++] = w >> 0;
			    int off = ((int) va[0].val - ((int) pc + 6)) / 2;
			    code[bytes++] = off >> 24;
			    code[bytes++] = off >> 16;
			    code[bytes++] = off >> 8;
			    code[bytes++] = off;
			} break;
		    case 'L': { // lea
			    code[bytes++] = w >> 8;
			    code[bytes++] = w >> 0 | (va[0].rno << 4);
			    int off = ((int) va[1].val - ((int) pc + 6));
			    code[bytes++] = off >> 24;
			    code[bytes++] = off >> 16;
			    code[bytes++] = off >> 8;
			    code[bytes++] = off;
			} break;
		    case 'l':
			if (va[1].rno == 15) {
			    // leasp
			    va[1].rno = va[0].rno;
			    va[0].rno = 0;
			    k	      = 'M';
			} else {
			    // sugar syntax for add
			    w	      = 0x3004;
			    va[2].val = va[1].val;
			    k	      = 'A';
			}
			goto again;
		    case 'I': { // li
			    int n = va[1].val;
			    if (n == 0) {
				// and r, zero, sp
				code[bytes++] = 0x80 | va[0].rno;
				code[bytes++] = 0xff;
			    } else if (n == 1) {
				// csetz r, sp
				code[bytes++] = 0x00 | va[0].rno;
				code[bytes++] = 0xf8;
			    } else if (n >= -32768 && n <= 32767) {
				// use ori
				w	  = 0x30f9;
				va[1].rno = 0;
				va[2].val = va[1].val;
				k	  = 'A';
				goto again;
			    } else {
				code[bytes++] = w >> 8;
				code[bytes++] = w >> 0 | (va[0].rno << 4);
				code[bytes++] = n >> 24;
				code[bytes++] = n >> 16;
				code[bytes++] = n >> 8;
				code[bytes++] = n;
			    }
			} break;
		    case '1':	// one register
			code[bytes++] = (w >> 8);
			code[bytes++] = (w >> 0) | (va[0].rno << 4);
			break;
		    case '=':	// mv
			code[bytes++] = (w >> 8) | va[0].rno;
			code[bytes++] = (w >> 0) | va[1].rno;
			break;
		    case 'G':	// get
			code[bytes++] = (w >> 8);
			code[bytes++] = (w >> 0) | (va[0].rno << 4);
			code[bytes++] = va[1].val >> 8;
			code[bytes++] = va[1].val;
			if (out && (va[1].val < 0 || va[1].val > 15))
			    error (lineno, "special register of range");
			break;
		    case 'g':	// set
			va[0].rno = va[1].rno;
			va[1].val = va[0].val;
			k	  = 'G';
			goto again;
		    default:
			//printf ("type %c\t%s\n", te->type, te->pattern);
			error (lineno, "opcode type");
			break;
		}
	    }
	}

	// print line
	if (listing) {
	    unsigned count = org ? 0 : bytes;
	    oprint (-1, fmt ("%w ", pc));
	    if (lbl && equ)
		oprint (-1, fmt ("= %w.%w ", lbl->value >> 16, lbl->value));
	    else if (space)
		oprint (-1, fmt ("? %w %5", space, space));
	    else {
		for (unsigned i = 0; i < 6; i++)
		    if (i < count) oprint (-1, fmt ("%b", code[i]));
			      else oprint (2, "  ");
	    }
	    oprint (-1, fmt (" %5\t%s", lineno, buffer));

	    if (count > 6 && !space)
		for (unsigned i = 6; i < count;) {
		    oprint (-1, fmt ("%w ", pc + i));
		    for (unsigned n = 0; n < 6; ++n, i++)
			if (i < count) oprint (-1, fmt ("%b", code[i]));
				  else oprint (2, "  ");
		    oprint (1, "\n");
		}
	}

	// output
	if (out && !org && bytes && !space)
	    for (unsigned i = 0; i < bytes; i++)
		emit (pc + i, code[i]);

	// update counter
	pc += bytes;

	// discard comments and empty lines
	if (!*p || *p == ';' || *p == '#')
	    continue;

	// error
	//printf ("\t%s", buffer);
	error (lineno, "extra characters at end");

	// next
	next: ;
    }
    return pc;
}

/*
 * entry point
 */
int main (int argc, char **argv) {
    // options
    bool listing = false;
    bool unused  = false;
    bool verbose = false;

    // command line options
    for (--argc, ++argv; argc && argv[0][0] == '-'; --argc, ++argv) {
	const char *op = *argv;
	if (!strcmp (op, "-l"))
	    listing = true;
	else if (!strcmp (op, "-u"))
	    unused = true;
	else if (!strcmp (op, "-v"))
	    verbose = true;
	else {
	    eprint (-1, fmt ("eonasm: unknown option [%s]\n", op));
	    exit   (1);
	}
    }

    // usage
    if (argc < 2) {
	oprint (-1,
	    "eonasm " VERSION ", classical assembler for eon cpu\n"
	    "usage  : eonasm [option]* outfile infile+\n"
	    "options:\n"
	    "\t-l\tlisting\n"
	    "\t-u\tshow unused labels\n"
	    "\t-v\tverbose assembly\n"
	    );
	exit (1);
    }

    // process infiles
    unsigned pass = 0;
    bool  another = true;
    bool  last	  = false;
    for (; !errcount && another; ++pass) {
	// verbose
	if (verbose) eprint (-1, fmt ("\tbegin pass %5%s\n", pass, last ? " (last)" : ""));

	// output file
	if (pass) output_to (argv[0]);

	// assemble
	unsigned pc = 0;
	bool   more = false;
	for (int i = 1; i < argc; ++i) {
	    source = argv[i];
	    int fd = open (source, O_RDONLY);
	    if (fd < 0) {
		eprint (-1, fmt ("error opening [%s]: %m\n", source));
		exit   (1);
	    }
	    if (last && listing) oprint (-1, fmt ("####################### %s\n", source));
	    pc = assemble (fd, pass, last, pc, last ? listing : false, &more);
	    close (fd);
	}

	// done
	if (last) emit_done ();

	// flags logic
	if (last)
	    another = false;
	else if (!more)
	    last = true;
    }

    // stats
    if (listing || errcount)
	oprint (-1, fmt ("####################### %5 passes. global/local labels (MAX %5): %5 / %5\n",
	    pass, MAX_LABELS, nlabel, MAX_LABELS - lstack
	    ));

    // error summary
    if (errcount) {
	eprint (-1, fmt ("eonasm: %5 errors.\n", errcount));
	return 1;
    }

    // dump unused labels
    if (unused)
	for (unsigned i = 0; i < nlabel; ++i) {
	    label_t l = &tlabel[i];
	    if (!(l->flags & LABEL_USED))
		eprint (-1, fmt ("eonasm: unused label [%s]\n", l->name));
	}

    // done
    return 0;
}
