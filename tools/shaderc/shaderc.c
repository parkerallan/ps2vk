/*
 * ps2vk-shaderc: compiles SPIR-V (from glslangValidator) for the PS2.
 *
 *   vertex shader   -> VU1 microcode (.vsm for dvp-as) + PvkShaderCode (.c)
 *   fragment shader -> GS settings in a PvkShaderCode (.c)
 *
 * This version accepts:
 *
 *   vertex:   gl_Position and an optional color output computed from vec4 inputs
 *             and float/vec4 constants with +, -, *, / and negation
 *   fragment: <color output> = <interpolated color input>;
 *
 * Anything else is rejected with an error that says why, the way a Vulkan
 * driver rejects a shader that uses a feature the device lacks.
 *
 * Usage: shaderc input.spv -o output_base
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps2vk_vu1_layout.h"

#define MAX_IDS     4096
#define MAX_INPUTS  8
#define MAX_OUTPUTS 8
#define MAX_BODY    256

/* SPIR-V opcodes used here (SPIR-V 1.x specification). */
enum {
    OpNop = 0, OpSource = 3, OpSourceExtension = 4, OpName = 5, OpMemberName = 6,
    OpString = 7, OpLine = 8, OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12,
    OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17,
    OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
    OpTypeMatrix = 24, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27,
    OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32,
    OpTypeFunction = 33, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43,
    OpConstantComposite = 44, OpFunction = 54, OpFunctionParameter = 55, OpFunctionEnd = 56,
    OpFunctionCall = 57, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
    OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80,
    OpCompositeExtract = 81, OpSampledImage = 86, OpImageSampleImplicitLod = 87,
    OpFNegate = 127, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136,
    OpVectorTimesScalar = 142, OpMatrixTimesVector = 145, OpDot = 148,
    OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249,
    OpBranchConditional = 250, OpKill = 252, OpReturn = 253, OpReturnValue = 254,
    OpNoLine = 317, OpModuleProcessed = 330
};

enum { DecBlock = 2, DecBuiltIn = 11, DecLocation = 30 };
enum { BuiltInPosition = 0 };
enum { ScInput = 1, ScOutput = 3, ScFunction = 7 };
enum { ExecVertex = 0, ExecFragment = 4 };

/* What we know about each SPIR-V result id. */
typedef enum { ID_NONE, ID_TYPE, ID_CONST, ID_VAR, ID_VALUE, ID_PTR } IdKind;
typedef enum { T_OTHER, T_FLOAT, T_INT, T_VECTOR, T_STRUCT, T_POINTER } TypeKind;

typedef struct {
    IdKind   kind;
    /* types */
    TypeKind type;
    uint32_t width;          /* float/int bits */
    uint32_t elemType;       /* vector component type, pointer pointee */
    uint32_t elemCount;      /* vector size */
    uint32_t storage;        /* pointer storage class */
    /* constants and values */
    uint32_t value;
    uint32_t valType;        /* type id of a constant or value */
    uint32_t comps[4];       /* constant components (float bits) */
    int      compCount;      /* 0 if the constant isn't a float or float vector */
    /* variables and pointers */
    uint32_t varType;        /* pointer type id */
    int      location;       /* -1 if none */
    int      builtin;        /* -1 if none */
    int      structPosMember;/* gl_PerVertex: member index decorated Position */
    int      isPosition;     /* pointer that targets gl_Position */
    /* values: which input a loaded value came from */
    int      fromInput;      /* index into inputs[], or -1 */
    const char *name;
} IdInfo;

typedef struct {
    int location;
    uint32_t var;
} Var;

static const char *g_src = "shader";
static const char *g_file = NULL;    /* from OpLine/OpString, if present */
static uint32_t g_line = 0;

static void fail(const char *fmt, ...)
{
    va_list ap;

    if (g_line)
        fprintf(stderr, "%s:%u: error: ", g_file ? g_file : g_src, (unsigned)g_line);
    else
        fprintf(stderr, "%s: error: ", g_src);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static const char *op_name(uint32_t op)
{
    switch (op) {
    case OpExtInst: return "OpExtInst (a GLSL built-in function)";
    case OpFunctionCall: return "OpFunctionCall (a call to another function)";
    case OpVectorShuffle: return "OpVectorShuffle (a swizzle)";
    case OpCompositeConstruct: return "OpCompositeConstruct (a vector constructor)";
    case OpCompositeExtract: return "OpCompositeExtract (a component read)";
    case OpSampledImage: case OpImageSampleImplicitLod: return "texture sampling";
    case OpFNegate: return "OpFNegate (negation)";
    case OpFAdd: return "OpFAdd (addition)";
    case OpFSub: return "OpFSub (subtraction)";
    case OpFMul: return "OpFMul (multiplication)";
    case OpFDiv: return "OpFDiv (division)";
    case OpVectorTimesScalar: return "OpVectorTimesScalar (vector * scalar)";
    case OpMatrixTimesVector: return "OpMatrixTimesVector (matrix * vector)";
    case OpDot: return "OpDot (dot product)";
    case OpBranch: case OpBranchConditional: case OpSelectionMerge: case OpLoopMerge:
        return "control flow (if/else or loops)";
    case OpKill: return "discard";
    default: return NULL;
    }
}

static void unsupported(uint32_t op, const char *stage)
{
    char what[96];
    const char *name = op_name(op);

    if (name)
        snprintf(what, sizeof(what), "%s", name);
    else
        snprintf(what, sizeof(what), "SPIR-V opcode %u", (unsigned)op);

    if (!strcmp(stage, "fragment"))
        fail("fragment shader uses %s: the GS cannot run code per pixel. "
             "Only 'outColor = <interpolated color input>;' is supported so far", what);
    fail("vertex shader uses %s, which this version of ps2vk-shaderc does not "
         "compile yet. Only +, -, *, / and negation of inputs and constants are supported so far", what);
}

static uint32_t *read_spirv(const char *path, size_t *words)
{
    FILE *f = fopen(path, "rb");
    long size;
    uint32_t *data;

    if (!f)
        fail("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 20 || size % 4)
        fail("%s is not a SPIR-V binary", path);
    data = malloc(size);
    if (!data || fread(data, 1, size, f) != (size_t)size)
        fail("cannot read %s", path);
    fclose(f);
    if (data[0] != 0x07230203)
        fail("%s is not a SPIR-V binary (bad magic number)", path);
    *words = size / 4;
    return data;
}

static int is_vec4_float(const IdInfo *ids, uint32_t type)
{
    const IdInfo *t = &ids[type];
    return t->type == T_VECTOR && t->elemCount == 4 &&
           ids[t->elemType].type == T_FLOAT && ids[t->elemType].width == 32;
}

static int is_float(const IdInfo *ids, uint32_t type)
{
    return ids[type].type == T_FLOAT && ids[type].width == 32;
}

/* Component count of an arithmetic operand or result: 1 (float) or 4 (vec4). */
static int value_size(const IdInfo *ids, uint32_t id, const char *stage)
{
    if (ids[id].kind != ID_VALUE && !(ids[id].kind == ID_CONST && ids[id].compCount))
        fail("%s shader computes with something other than inputs and constants", stage);
    if (is_float(ids, ids[id].valType))
        return 1;
    if (is_vec4_float(ids, ids[id].valType))
        return 4;
    fail("%s shader computes with a type other than float or vec4; only those are supported so far", stage);
    return 0;
}

/* ----------------------------------------------------------------------- */

/* One arithmetic instruction of a vertex shader body, in program order. */
typedef struct {
    uint32_t op;
    uint32_t result;
    uint32_t a, b;                  /* operand ids; b is 0 for OpFNegate */
    uint32_t line;                  /* GLSL line, 0 if unknown */
} BodyOp;

typedef struct {
    uint32_t execModel;
    Var      inputs[MAX_INPUTS];
    int      inputCount;
    Var      outputs[MAX_OUTPUTS];  /* location outputs only */
    int      outputCount;
    uint32_t positionVar;           /* gl_PerVertex or Position variable, 0 if none */
    uint32_t positionValue;         /* value id stored to gl_Position, 0 if never */
    uint32_t colorValue;            /* value id stored to the color output, 0 if never */
    int      colorLocation;         /* -1 if none */
    BodyOp   body[MAX_BODY];
    int      bodyCount;
} Shader;

static void parse(const uint32_t *code, size_t words, IdInfo *ids, Shader *sh)
{
    size_t pc;
    uint32_t bound = code[3];
    int inFunction = 0, functions = 0;

    if (bound > MAX_IDS)
        fail("shader is too large (%u ids)", (unsigned)bound);

    memset(sh, 0, sizeof(*sh));
    sh->execModel = ~0u;
    sh->colorLocation = -1;
    for (uint32_t i = 0; i < bound; i++) {
        ids[i].location = ids[i].builtin = ids[i].structPosMember = -1;
        ids[i].fromInput = -1;
    }

    /* Pass 1: decorations, types, constants and global variables. */
    for (pc = 5; pc < words;) {
        uint32_t op = code[pc] & 0xffff, n = code[pc] >> 16;
        const uint32_t *w = &code[pc];

        if (n == 0 || pc + n > words)
            fail("corrupt SPIR-V");

        switch (op) {
        case OpCapability:
            if (w[1] != 1) /* Shader */
                fail("shader needs SPIR-V capability %u, which the PS2 does not support", (unsigned)w[1]);
            break;
        case OpEntryPoint:
            if (sh->execModel != ~0u)
                fail("only one entry point per file is supported");
            sh->execModel = w[1];
            break;
        case OpString:
            ids[w[1]].name = (const char *)&w[2];
            break;
        case OpName:
            ids[w[1]].name = (const char *)&w[2];
            break;
        case OpDecorate:
            if (w[2] == DecLocation)
                ids[w[1]].location = (int)w[3];
            else if (w[2] == DecBuiltIn)
                ids[w[1]].builtin = (int)w[3];
            break;
        case OpMemberDecorate:
            if (w[3] == DecBuiltIn && w[4] == BuiltInPosition)
                ids[w[1]].structPosMember = (int)w[2];
            break;
        case OpTypeFloat:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_FLOAT; ids[w[1]].width = w[2];
            break;
        case OpTypeInt:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_INT; ids[w[1]].width = w[2];
            break;
        case OpTypeVector:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_VECTOR;
            ids[w[1]].elemType = w[2]; ids[w[1]].elemCount = w[3];
            break;
        case OpTypeStruct:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_STRUCT;
            break;
        case OpTypePointer:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_POINTER;
            ids[w[1]].storage = w[2]; ids[w[1]].elemType = w[3];
            break;
        case OpTypeImage: case OpTypeSampler: case OpTypeSampledImage: case OpTypeMatrix:
        case OpTypeVoid: case OpTypeBool: case OpTypeArray: case OpTypeRuntimeArray:
        case OpTypeFunction:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_OTHER;
            break;
        case OpConstant:
            ids[w[2]].kind = ID_CONST; ids[w[2]].value = w[3];
            ids[w[2]].valType = w[1];
            if (is_float(ids, w[1])) {
                ids[w[2]].comps[0] = w[3];
                ids[w[2]].compCount = 1;
            }
            break;
        case OpConstantComposite: {
            uint32_t id = w[2];
            ids[id].kind = ID_CONST;
            ids[id].valType = w[1];
            if (ids[w[1]].type == T_VECTOR && is_float(ids, ids[w[1]].elemType) && n - 3 <= 4) {
                for (uint32_t i = 0; i < n - 3; i++)
                    ids[id].comps[i] = ids[w[3 + i]].comps[0];
                ids[id].compCount = (int)(n - 3);
            }
            break;
        }
        case OpVariable: {
            uint32_t type = w[1], id = w[2], storage = w[3];
            ids[id].kind = ID_VAR;
            ids[id].varType = type;
            if (!inFunction && storage != ScInput && storage != ScOutput)
                fail("'%s' is a uniform, push constant or buffer; this version of "
                     "ps2vk-shaderc supports only shader inputs and outputs",
                     ids[id].name ? ids[id].name : "variable");
            if (inFunction && storage == ScFunction)
                fail("local variables are not supported yet");
            break;
        }
        case OpFunction:
            inFunction = 1;
            functions++;
            break;
        case OpFunctionEnd:
            inFunction = 0;
            break;
        default:
            break;
        }
        pc += n;
    }

    if (sh->execModel != ExecVertex && sh->execModel != ExecFragment)
        fail("only vertex and fragment shaders are supported");
    if (functions != 1)
        fail("shader has %d functions; calls to other functions are not supported yet", functions);

    /* Collect the interface variables (Input/Output OpVariables at global scope). */
    for (pc = 5; pc < words;) {
        uint32_t op = code[pc] & 0xffff, n = code[pc] >> 16;
        const uint32_t *w = &code[pc];

        if (op == OpFunction)
            break;
        if (op == OpVariable) {
            uint32_t id = w[2], storage = w[3];
            uint32_t pointee = ids[w[1]].elemType;
            const char *name = ids[id].name ? ids[id].name : "variable";

            if (storage == ScInput) {
                if (ids[id].builtin >= 0)
                    fail("built-in input '%s' is not supported", name);
                if (ids[id].location < 0)
                    fail("input '%s' has no location", name);
                if (!is_vec4_float(ids, pointee))
                    fail("input '%s' must be a vec4 for now", name);
                if (sh->inputCount == MAX_INPUTS)
                    fail("too many inputs (max %d)", MAX_INPUTS);
                sh->inputs[sh->inputCount].location = ids[id].location;
                sh->inputs[sh->inputCount].var = id;
                sh->inputCount++;
            } else if (storage == ScOutput) {
                if (ids[pointee].type == T_STRUCT && ids[pointee].structPosMember >= 0) {
                    sh->positionVar = id; /* gl_PerVertex block */
                } else if (ids[id].builtin == BuiltInPosition) {
                    sh->positionVar = id;
                    ids[id].isPosition = 1;
                } else if (ids[id].builtin >= 0) {
                    fail("built-in output '%s' is not supported", name);
                } else {
                    if (ids[id].location < 0)
                        fail("output '%s' has no location", name);
                    if (!is_vec4_float(ids, pointee))
                        fail("output '%s' must be a vec4 for now", name);
                    if (sh->outputCount == MAX_OUTPUTS)
                        fail("too many outputs");
                    sh->outputs[sh->outputCount].location = ids[id].location;
                    sh->outputs[sh->outputCount].var = id;
                    sh->outputCount++;
                }
            }
        }
        pc += n;
    }

    /* Sort inputs by location: VU1 reads them in that order. */
    for (int i = 0; i < sh->inputCount; i++)
        for (int j = i + 1; j < sh->inputCount; j++)
            if (sh->inputs[j].location < sh->inputs[i].location) {
                Var t = sh->inputs[i]; sh->inputs[i] = sh->inputs[j]; sh->inputs[j] = t;
            }
    for (int i = 0; i < sh->inputCount; i++)
        ids[sh->inputs[i].var].fromInput = i;

    if (sh->outputCount > 1)
        fail("shader has %d color outputs; the GS can interpolate only one color", sh->outputCount);
    if (sh->outputCount == 1)
        sh->colorLocation = sh->outputs[0].location;
    if (sh->execModel == ExecFragment) {
        if (sh->outputCount != 1 || sh->colorLocation != 0)
            fail("fragment shader must have exactly one vec4 output at location 0");
        if (sh->inputCount > 1)
            fail("fragment shader has %d inputs; the GS interpolates only one color", sh->inputCount);
    }

    /* Pass 2: the function body. */
    inFunction = 0;
    for (pc = 5; pc < words;) {
        uint32_t op = code[pc] & 0xffff, n = code[pc] >> 16;
        const uint32_t *w = &code[pc];
        const char *stage = sh->execModel == ExecVertex ? "vertex" : "fragment";

        if (op == OpLine) {
            g_file = ids[w[1]].name;
            g_line = w[2];
        } else if (op == OpNoLine) {
            g_line = 0;
        }

        if (op == OpFunction) {
            inFunction = 1;
        } else if (inFunction) {
            switch (op) {
            case OpLabel: case OpReturn: case OpLine: case OpNoLine: case OpNop:
            case OpFunctionEnd:
                break;
            case OpLoad: {
                uint32_t id = w[2], ptr = w[3];
                if (ids[ptr].kind != ID_VAR || ids[ptr].fromInput < 0)
                    fail("%s shader reads something other than a vertex input", stage);
                ids[id].kind = ID_VALUE;
                ids[id].valType = w[1];
                ids[id].fromInput = ids[ptr].fromInput;
                break;
            }
            case OpFAdd: case OpFSub: case OpFMul: case OpFDiv: case OpFNegate:
            case OpVectorTimesScalar: {
                BodyOp *b;
                int size;

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                ids[w[2]].kind = ID_VALUE;
                ids[w[2]].valType = w[1];
                size = value_size(ids, w[2], stage);
                if (op == OpVectorTimesScalar) {
                    if (value_size(ids, w[3], stage) != 4 || value_size(ids, w[4], stage) != 1)
                        fail("corrupt SPIR-V: OpVectorTimesScalar needs a vec4 and a float");
                } else if (value_size(ids, w[3], stage) != size ||
                           (op != OpFNegate && value_size(ids, w[4], stage) != size)) {
                    fail("corrupt SPIR-V: %s operands differ in size", op_name(op));
                }
                if (sh->bodyCount == MAX_BODY)
                    fail("vertex shader is too long (max %d instructions)", MAX_BODY);
                b = &sh->body[sh->bodyCount++];
                b->op = op;
                b->result = w[2];
                b->a = w[3];
                b->b = op == OpFNegate ? 0 : w[4];
                b->line = g_line;
                break;
            }
            case OpCompositeConstruct: {
                /* Only all-constant vectors, e.g. glslang's vec4(2.0) for 'v / 2.0'. */
                uint32_t id = w[2];
                if (sh->execModel != ExecVertex || n - 3 > 4)
                    unsupported(op, stage);
                for (uint32_t i = 0; i < n - 3; i++) {
                    if (ids[w[3 + i]].kind != ID_CONST || ids[w[3 + i]].compCount != 1)
                        unsupported(op, stage);
                    ids[id].comps[i] = ids[w[3 + i]].comps[0];
                }
                ids[id].kind = ID_CONST;
                ids[id].valType = w[1];
                ids[id].compCount = (int)(n - 3);
                break;
            }
            case OpAccessChain: {
                uint32_t id = w[2], base = w[3];
                if (base != sh->positionVar || n != 5 || ids[w[4]].kind != ID_CONST)
                    fail("%s shader accesses a member or element other than gl_Position", stage);
                {
                    uint32_t structType = ids[ids[base].varType].elemType;
                    if ((int)ids[w[4]].value != ids[structType].structPosMember)
                        fail("only gl_Position can be written in gl_PerVertex");
                }
                ids[id].kind = ID_PTR;
                ids[id].isPosition = 1;
                break;
            }
            case OpStore: {
                uint32_t ptr = w[1], val = w[2];

                if (sh->execModel == ExecFragment) {
                    if (ids[val].kind != ID_VALUE || ids[val].fromInput < 0)
                        fail("%s shader stores a computed value; only copying an input "
                             "unchanged is supported so far", stage);
                } else if (value_size(ids, val, stage) != 4) {
                    fail("%s shader stores something other than a vec4 to an output", stage);
                }
                if (ids[ptr].isPosition) {
                    sh->positionValue = val;
                } else if (sh->outputCount == 1 && ptr == sh->outputs[0].var) {
                    sh->colorValue = val;
                } else {
                    fail("%s shader writes to something other than its outputs", stage);
                }
                break;
            }
            default:
                unsupported(op, stage);
            }
        }
        pc += n;
    }
    g_line = 0;

    if (sh->execModel == ExecVertex) {
        if (!sh->positionValue)
            fail("vertex shader never writes gl_Position");
        if (sh->outputCount == 1 && !sh->colorValue)
            fail("vertex shader declares a color output but never writes it");
    } else if (!sh->colorValue) {
        fail("fragment shader must write 'outColor = <color input>;'");
    }
}

/* ----------------------------------------------------------------------- */

static FILE *open_out(const char *base, const char *ext)
{
    char path[1024];
    FILE *f;

    snprintf(path, sizeof(path), "%s%s", base, ext);
    f = fopen(path, "w");
    if (!f)
        fail("cannot write %s", path);
    return f;
}

/* Upper (FMAC) and lower (load/store/integer/branch) instruction of one VU1 cycle. */
static void vu(FILE *f, const char *upper, const char *lower, const char *comment)
{
    if (comment)
        fprintf(f, "         %-40s %-40s ; %s\n", upper, lower, comment);
    else
        fprintf(f, "         %-40s %s\n", upper, lower);
}

/*
 * VF registers free for constants and shader temporaries. The rest hold the
 * prologue constants (VF01-VF04), inputs (VF10-VF17) and epilogue values (VF20-VF24).
 */
static const int g_pool[] = { 5, 6, 7, 8, 9, 18, 19, 25, 26, 27, 28, 29, 30, 31 };

typedef struct {
    int reg[MAX_IDS];       /* VF register holding each value, 0 if none */
    int lastUse[MAX_IDS];   /* body index of the last read, MAX_BODY if stored to an output */
    int busy[32];
} Regs;

static int reg_alloc(Regs *r)
{
    for (size_t i = 0; i < sizeof(g_pool) / sizeof(g_pool[0]); i++)
        if (!r->busy[g_pool[i]]) {
            r->busy[g_pool[i]] = 1;
            return g_pool[i];
        }
    fail("vertex shader needs more than %d values at once; VU1 has run out of registers",
         (int)(sizeof(g_pool) / sizeof(g_pool[0])));
    return 0;
}

/* Frees a temporary after its last read (inputs and constants stay). */
static void reg_release(Regs *r, const IdInfo *ids, uint32_t id, int at)
{
    if (ids[id].kind == ID_VALUE && ids[id].fromInput < 0 && r->lastUse[id] <= at && r->reg[id]) {
        r->busy[r->reg[id]] = 0;
        r->reg[id] = 0;
    }
}

/* LOI takes the float's raw bits in hex, as VCL's output does; the comment gets the value. */
static void loi_text(uint32_t bits, char *ins, size_t insSize, char *comment, size_t commentSize)
{
    float v;

    memcpy(&v, &bits, sizeof(v));
    if (v != v || v - v != 0)
        fail("constant is infinite or NaN, which VU1 cannot represent");
    snprintf(ins, insSize, "loi           0x%08x", (unsigned)bits);
    snprintf(comment, commentSize, "%.9g", v);
}

/* Loads a constant into its register: zero it, then LOI + ADDI per distinct component value. */
static void emit_constant(FILE *f, const IdInfo *c, int reg)
{
    static const char field[] = "xyzw";
    char buf[64], buf2[64], mask[5], val[32];
    int done[4] = { 0 }, i, j, k;

    snprintf(buf, sizeof(buf), "sub.xyzw      VF%02d,VF00,VF00", reg);
    snprintf(buf2, sizeof(buf2), "%s", c->compCount == 1 ? "float constant" : "vec4 constant");
    vu(f, buf, "NOP", buf2);
    for (i = 0; i < c->compCount; i++) {
        if (done[i] || !(c->comps[i] & 0x7fffffff))
            continue;
        for (j = i, k = 0; j < c->compCount; j++)
            if (c->comps[j] == c->comps[i]) {
                mask[k++] = field[j];
                done[j] = 1;
            }
        mask[k] = 0;
        loi_text(c->comps[i], buf, sizeof(buf), val, sizeof(val));
        vu(f, "NOP", buf, val);
        snprintf(buf, sizeof(buf), "addi.%-8s VF%02d,VF%02d,I", mask, reg, reg);
        vu(f, buf, "NOP", NULL);
    }
}

static void emit_body_op(FILE *f, const IdInfo *ids, const BodyOp *b, int d, int a, int bb)
{
    static const char field[] = "xyzw";
    const char *dest = is_float(ids, ids[b->result].valType) ? "x" : "xyzw";
    char buf[64], line[96];
    int i;

    if (b->line)
        snprintf(line, sizeof(line), "line %u: %s", (unsigned)b->line, op_name(b->op));
    else
        snprintf(line, sizeof(line), "%s", op_name(b->op));

    switch (b->op) {
    case OpFAdd: case OpFSub: case OpFMul: {
        const char *m = b->op == OpFAdd ? "add" : b->op == OpFSub ? "sub" : "mul";
        char mn[16];
        snprintf(mn, sizeof(mn), "%s.%s", m, dest);
        snprintf(buf, sizeof(buf), "%-14sVF%02d,VF%02d,VF%02d", mn, d, a, bb);
        vu(f, buf, "NOP", line);
        break;
    }
    case OpVectorTimesScalar:
        snprintf(buf, sizeof(buf), "mulx.xyzw     VF%02d,VF%02d,VF%02dx", d, a, bb);
        vu(f, buf, "NOP", line);
        break;
    case OpFNegate: {
        char mn[16];
        vu(f, "NOP", "loi           0xbf800000", line);   /* -1.0 */
        snprintf(mn, sizeof(mn), "muli.%s", dest);
        snprintf(buf, sizeof(buf), "%-14sVF%02d,VF%02d,I", mn, d, a);
        vu(f, buf, "NOP", NULL);
        break;
    }
    case OpFDiv:
        /* VU1 divides one component at a time into Q; copy Q out with VF00 = (0, 0, 0, 1). */
        for (i = 0; i < (int)strlen(dest); i++) {
            char c = field[i];
            snprintf(buf, sizeof(buf), "div           Q,VF%02d%c,VF%02d%c", a, c, bb, c);
            vu(f, "NOP", buf, i ? NULL : line);
            vu(f, "NOP", "waitq", NULL);
            if (c == 'w')
                snprintf(buf, sizeof(buf), "mulq.w        VF%02d,VF00,Q", d);
            else
                snprintf(buf, sizeof(buf), "addq.%c        VF%02d,VF00,Q", c, d);
            vu(f, buf, "NOP", NULL);
        }
        break;
    }
}

static void emit_vertex(const Shader *sh, const IdInfo *ids, const char *base, const char *sym)
{
    FILE *f = open_out(base, ".vsm");
    char buf[64], buf2[64];
    static Regs r;
    int i, constants = 0;

    /* Registers: inputs are fixed, constants live for the whole program, temporaries
     * are freed after their last read. */
    memset(&r, 0, sizeof(r));
    for (i = 0; i < sh->bodyCount; i++) {
        r.lastUse[sh->body[i].a] = i;
        if (sh->body[i].b)
            r.lastUse[sh->body[i].b] = i;
    }
    r.lastUse[sh->positionValue] = MAX_BODY;
    if (sh->colorValue)
        r.lastUse[sh->colorValue] = MAX_BODY;
    for (i = 0; i < MAX_IDS; i++)
        if (ids[i].kind == ID_VALUE && ids[i].fromInput >= 0)
            r.reg[i] = 10 + ids[i].fromInput;

    fprintf(f, "; Generated by ps2vk-shaderc from %s. Do not edit.\n", g_src);
    fprintf(f, ";\n; VU1 registers:\n");
    fprintf(f, ";   VF01 viewport scale, VF02 viewport offset, VF03 color scale\n");
    fprintf(f, ";   VF10.. shader inputs, VF20 gl_Position, VF21 color output\n");
    fprintf(f, ";   VF05-VF09, VF18-VF19, VF25-VF31 constants and temporaries\n");
    fprintf(f, ";   VI02 vertices left, VI03 input pointer, VI04 output base, VI05 output pointer\n\n");
    fprintf(f, "\t\t.vu\n\t\t.align 4\n");
    fprintf(f, "\t\t.global\t%s_CodeStart\n\t\t.global\t%s_CodeEnd\n", sym, sym);
    fprintf(f, "%s_CodeStart:\n", sym);

    fprintf(f, "; prologue: constants, batch header, GIF tag to the output\n");
    snprintf(buf, sizeof(buf), "lq            VF01,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_VIEWPORT_SCALE);
    vu(f, "NOP", buf, NULL);
    snprintf(buf, sizeof(buf), "lq            VF02,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_VIEWPORT_OFFSET);
    vu(f, "NOP", buf, NULL);
    snprintf(buf, sizeof(buf), "lq            VF03,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_COLOR_SCALE);
    vu(f, "NOP", buf, NULL);
    snprintf(buf, sizeof(buf), "iaddiu        VI01,VI00,%d", PVK_VU1_INPUT_ADDR);
    vu(f, "NOP", buf, "input base");
    snprintf(buf, sizeof(buf), "lq            VF04,%d(VI01)", PVK_VU1_INPUT_GIFTAG);
    vu(f, "NOP", buf, "GIF tag for this batch");
    snprintf(buf, sizeof(buf), "ilw.x         VI02,%d(VI01)", PVK_VU1_INPUT_COUNT);
    vu(f, "NOP", buf, "vertex count");
    snprintf(buf, sizeof(buf), "iaddiu        VI03,VI01,%d", PVK_VU1_INPUT_HEADER);
    vu(f, "NOP", buf, "first vertex");
    snprintf(buf, sizeof(buf), "iaddiu        VI04,VI00,%d", PVK_VU1_OUTPUT_ADDR);
    vu(f, "NOP", buf, "output base");
    vu(f, "sub.xyzw      VF23,VF00,VF00", "sq            VF04,0(VI04)", "XYZ2 w = 0 (ADC off)");
    vu(f, "NOP", "iaddiu        VI05,VI04,1", "output pointer");

    for (i = 0; i <= sh->bodyCount + 1; i++) {
        uint32_t use[2] = { 0, 0 };
        if (i < sh->bodyCount) {
            use[0] = sh->body[i].a;
            use[1] = sh->body[i].b;
        } else {
            use[0] = i == sh->bodyCount ? sh->positionValue : sh->colorValue;
        }
        for (int j = 0; j < 2; j++) {
            if (!use[j] || ids[use[j]].kind != ID_CONST || r.reg[use[j]])
                continue;
            if (!constants++)
                fprintf(f, "; constants\n");
            r.reg[use[j]] = reg_alloc(&r);
            emit_constant(f, &ids[use[j]], r.reg[use[j]]);
        }
    }

    fprintf(f, "%s_loop:\n", sym);
    fprintf(f, "; load inputs (location order)\n");
    for (i = 0; i < sh->inputCount; i++) {
        snprintf(buf, sizeof(buf), "lq            VF%02d,%d(VI03)", 10 + i, i);
        snprintf(buf2, sizeof(buf2), "location %d", sh->inputs[i].location);
        vu(f, "NOP", buf, buf2);
    }

    fprintf(f, "; shader body\n");
    for (i = 0; i < sh->bodyCount; i++) {
        const BodyOp *b = &sh->body[i];
        int a = r.reg[b->a], bb = b->b ? r.reg[b->b] : 0, d;

        reg_release(&r, ids, b->a, i);
        if (b->b)
            reg_release(&r, ids, b->b, i);
        d = r.reg[b->result] = reg_alloc(&r);
        emit_body_op(f, ids, b, d, a, bb);
        reg_release(&r, ids, b->result, i);   /* never read */
    }
    snprintf(buf, sizeof(buf), "move.xyzw     VF20,VF%02d", r.reg[sh->positionValue]);
    vu(f, "NOP", buf, "gl_Position");
    if (sh->colorValue) {
        snprintf(buf, sizeof(buf), "move.xyzw     VF21,VF%02d", r.reg[sh->colorValue]);
        snprintf(buf2, sizeof(buf2), "color output, location %d", sh->colorLocation);
        vu(f, "NOP", buf, buf2);
    } else {
        vu(f, "NOP", "move.xyzw     VF21,VF00", "no color output: black");
    }

    fprintf(f, "; epilogue: perspective divide, viewport, GS formats, store\n");
    vu(f, "NOP", "div           Q,VF00w,VF20w", "Q = 1 / w");
    vu(f, "NOP", "waitq", NULL);
    vu(f, "mulq.xyz      VF22,VF20,Q", "NOP", NULL);
    vu(f, "mul.xyz       VF22,VF22,VF01", "NOP", "viewport scale");
    vu(f, "add.xyz       VF22,VF22,VF02", "NOP", "viewport offset");
    vu(f, "ftoi4.xy      VF23,VF22", "NOP", "x, y to 12.4 fixed point");
    vu(f, "ftoi0.z       VF23,VF22", "NOP", "z to integer depth");
    vu(f, "mul.xyzw      VF24,VF21,VF03", "NOP", "color 0..1 to GS range");
    vu(f, "ftoi0.xyzw    VF24,VF24", "NOP", NULL);
    vu(f, "NOP", "sq            VF24,0(VI05)", "RGBAQ");
    vu(f, "NOP", "sq            VF23,1(VI05)", "XYZ2 (draws)");
    snprintf(buf, sizeof(buf), "iaddiu        VI05,VI05,%d", PVK_VU1_OUTPUT_PER_VERTEX);
    vu(f, "NOP", buf, NULL);
    snprintf(buf, sizeof(buf), "iaddiu        VI03,VI03,%d", sh->inputCount);
    vu(f, "NOP", buf, NULL);
    vu(f, "NOP", "isubiu        VI02,VI02,1", NULL);
    vu(f, "NOP", "NOP", NULL);
    snprintf(buf, sizeof(buf), "ibne          VI02,VI00,%s_loop", sym);
    vu(f, "NOP", buf, NULL);
    vu(f, "NOP", "NOP", "branch delay slot");

    fprintf(f, "; send the batch to the GS and stop\n");
    vu(f, "NOP", "xgkick        VI04", NULL);
    vu(f, "NOP[E]", "NOP", NULL);
    vu(f, "NOP", "NOP", NULL);
    fprintf(f, "\t\t.align 4\n%s_CodeEnd:\n", sym);
    fclose(f);

    f = open_out(base, ".c");
    fprintf(f, "/* Generated by ps2vk-shaderc from %s. Do not edit. */\n", g_src);
    fprintf(f, "#include \"ps2vk.h\"\n\n");
    fprintf(f, "extern uint32_t %s_CodeStart __attribute__((section(\".vudata\")));\n", sym);
    fprintf(f, "extern uint32_t %s_CodeEnd __attribute__((section(\".vudata\")));\n\n", sym);
    fprintf(f, "const PvkShaderCode %s = {\n", sym);
    fprintf(f, "    .stage = PVK_SHADER_STAGE_VERTEX,\n");
    fprintf(f, "    .name = \"%s\",\n", g_src);
    fprintf(f, "    .vu1CodeStart = &%s_CodeStart,\n", sym);
    fprintf(f, "    .vu1CodeEnd = &%s_CodeEnd,\n", sym);
    fprintf(f, "    .inputCount = %d,\n", sh->inputCount);
    fprintf(f, "    .inputLocations = {");
    for (i = 0; i < sh->inputCount; i++)
        fprintf(f, "%s%d", i ? ", " : " ", sh->inputs[i].location);
    fprintf(f, " },\n");
    fprintf(f, "    .colorOutputLocation = %d,\n", sh->colorValue ? sh->colorLocation : -1);
    fprintf(f, "    .colorInputLocation = -1,\n");
    fprintf(f, "};\n");
    fclose(f);
}

static void emit_fragment(const Shader *sh, const IdInfo *ids, const char *base, const char *sym)
{
    FILE *f = open_out(base, ".c");

    fprintf(f, "/* Generated by ps2vk-shaderc from %s. Do not edit. */\n", g_src);
    fprintf(f, "#include \"ps2vk.h\"\n\n");
    fprintf(f, "/* outColor = interpolated color: Gouraud shading, no texture. */\n");
    fprintf(f, "const PvkShaderCode %s = {\n", sym);
    fprintf(f, "    .stage = PVK_SHADER_STAGE_FRAGMENT,\n");
    fprintf(f, "    .name = \"%s\",\n", g_src);
    fprintf(f, "    .colorOutputLocation = -1,\n");
    fprintf(f, "    .colorInputLocation = %d,\n", sh->inputs[ids[sh->colorValue].fromInput].location);
    fprintf(f, "    .gouraud = 1,\n");
    fprintf(f, "    .textured = 0,\n");
    fprintf(f, "};\n");
    fclose(f);
}

/* C identifier from the output path's file name: "build/tri.vert" -> "tri_vert". */
static void symbol_name(const char *base, char *out, size_t size)
{
    const char *s = strrchr(base, '/');
    size_t i;

    s = s ? s + 1 : base;
    for (i = 0; s[i] && i + 1 < size; i++) {
        char c = s[i];
        out[i] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
    }
    out[i] = 0;
    if (out[0] >= '0' && out[0] <= '9')
        out[0] = '_';
}

int main(int argc, char **argv)
{
    const char *in = NULL, *outBase = NULL;
    static IdInfo ids[MAX_IDS];
    uint32_t *code;
    size_t words;
    Shader sh;
    char sym[256];
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            outBase = argv[++i];
        else
            in = argv[i];
    }
    if (!in || !outBase) {
        fprintf(stderr, "usage: shaderc input.spv -o output_base\n");
        return 2;
    }

    {
        /* Name shaders after their GLSL source: "build/tri.vert.spv" -> "tri.vert". */
        static char src[256];
        const char *slash = strrchr(in, '/');
        size_t len;

        snprintf(src, sizeof(src), "%s", slash ? slash + 1 : in);
        len = strlen(src);
        if (len > 4 && !strcmp(src + len - 4, ".spv"))
            src[len - 4] = 0;
        g_src = src;
    }
    symbol_name(outBase, sym, sizeof(sym));

    code = read_spirv(in, &words);
    parse(code, words, ids, &sh);

    if (sh.execModel == ExecVertex)
        emit_vertex(&sh, ids, outBase, sym);
    else
        emit_fragment(&sh, ids, outBase, sym);

    free(code);
    return 0;
}
