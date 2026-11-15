/*
 * ps2vk-shaderc: compiles SPIR-V (glslangValidator -V) for the PS2.
 *
 *   .vert --glslangValidator--> .spv --shaderc--> .vsm + .c --dvp-as--> .o + .c  (VU1 microcode + PvkShaderCode)
 *   .frag --glslangValidator--> .spv --shaderc--> .c                            (GS settings in a PvkShaderCode)
 *
 * shaderc first runs spirv-opt (SPIRV-Tools) on its input to inline function
 * calls, turn local variables into plain values and unroll loops with a
 * constant count, so the body it compiles is a few blocks of code over ids
 * that live in registers, joined by branches and phis.
 *
 * This version accepts:
 *
 *   vertex:   gl_Position, an optional vec4 color output and an optional vec2
 *             texture coordinate output computed from vec4 inputs, push
 *             constants (a block of float/vec2/vec3/vec4/matrix members and
 *             arrays of vectors or matrices, up to 128 bytes), uniform blocks
 *             (the same members, std140, up to 1 KB together, each a
 *             descriptor set binding), array elements and matrix columns
 *             picked by an int block member, gl_InstanceIndex or int() of a
 *             float, and float/vec4/matrix constants with +, -, *, / and
 *             negation, matrix * vector, swizzles (v.xyz, v.bgra), component
 *             reads (v.w) and writes (v.w = x), vector constructors, math
 *             built-ins, local variables (vectors, matrices, structs and
 *             arrays indexed by constants), calls to the shader's own
 *             functions, int counters and arithmetic (+, -, * as floats),
 *             comparisons with &&, || and !, if/else, ?: and loops (unrolled
 *             when the count is constant, else VU1 branches)
 *   fragment: outColor = vColor;                      (Gouraud)
 *             outColor = texture(tex, vUV);           (GS TFX decal)
 *             outColor = texture(tex, vUV) * vColor;  (GS TFX modulate)
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

#include "ps2vk.h"
#include "ps2vk_vu1_layout.h"

#define MAX_IDS     4096
#define MAX_INPUTS  8
#define MAX_BODY    256
#define MAX_OPERANDS 8
#define MAX_PHIS    64
#define MAX_PHI_PREDS 8

/* SPIR-V opcodes used here (SPIR-V 1.x specification). */
enum {
    OpNop = 0, OpSource = 3, OpSourceExtension = 4, OpName = 5, OpMemberName = 6,
    OpString = 7, OpLine = 8, OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12,
    OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17,
    OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
    OpTypeMatrix = 24, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27,
    OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32,
    OpTypeFunction = 33, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43,
    OpConvertFToU = 109, OpConvertFToS = 110, OpConvertSToF = 111, OpConvertUToF = 112,
    OpBitcast = 124, OpSNegate = 126, OpIAdd = 128, OpISub = 130, OpIMul = 132,
    OpUDiv = 134, OpSDiv = 135, OpUMod = 137, OpSRem = 138, OpSMod = 139,
    OpConstantComposite = 44, OpFunction = 54, OpFunctionParameter = 55, OpFunctionEnd = 56,
    OpFunctionCall = 57, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
    OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80,
    OpCompositeExtract = 81, OpCompositeInsert = 82, OpTranspose = 84, OpSampledImage = 86,
    OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88,
    OpFNegate = 127, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136,
    OpVectorTimesScalar = 142, OpMatrixTimesScalar = 143, OpVectorTimesMatrix = 144,
    OpMatrixTimesVector = 145, OpMatrixTimesMatrix = 146, OpOuterProduct = 147, OpDot = 148,
    OpAny = 154, OpAll = 155, OpLogicalOr = 166, OpLogicalAnd = 167, OpLogicalNot = 168,
    OpSelect = 169, OpIEqual = 170, OpINotEqual = 171, OpUGreaterThan = 172,
    OpSGreaterThan = 173, OpUGreaterThanEqual = 174, OpSGreaterThanEqual = 175,
    OpULessThan = 176, OpSLessThan = 177, OpULessThanEqual = 178, OpSLessThanEqual = 179,
    OpFOrdEqual = 180, OpFUnordEqual = 181, OpFOrdNotEqual = 182, OpFUnordNotEqual = 183,
    OpFOrdLessThan = 184, OpFUnordLessThan = 185, OpFOrdGreaterThan = 186,
    OpFUnordGreaterThan = 187, OpFOrdLessThanEqual = 188, OpFUnordLessThanEqual = 189,
    OpFOrdGreaterThanEqual = 190, OpFUnordGreaterThanEqual = 191,
    OpShiftRightLogical = 194, OpBitwiseXor = 198, OpNot = 200,
    OpPhi = 245, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249,
    OpBranchConditional = 250, OpSwitch = 251, OpKill = 252, OpReturn = 253, OpReturnValue = 254,
    OpUnreachable = 255, OpNoLine = 317, OpModuleProcessed = 330,
    OpDynLoad = 0x10000,            /* ours: lq of a block qword through VI08 */
    OpPhiMove = 0x10001,            /* ours: copy a value into a phi's register at the end of a block */
    OpConstLoad = 0x10002,          /* ours: lq of a push constant or uniform qword */
    OpBoolToFloat = 0x10003         /* ours: a condition as a float, 0 = false, for a bool kept by a phi */
};
#define LoopControlUnroll     1
#define LoopControlDontUnroll 2

enum { DecBlock = 2, DecRowMajor = 4, DecColMajor = 5, DecArrayStride = 6, DecMatrixStride = 7,
       DecBuiltIn = 11, DecLocation = 30, DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35 };
enum { BuiltInPosition = 0, BuiltInInstanceIndex = 43 };
enum { ScUniformConstant = 0, ScInput = 1, ScUniform = 2, ScOutput = 3, ScFunction = 7,
       ScPushConstant = 9, ScStorageBuffer = 12 };
enum { ExecVertex = 0, ExecFragment = 4 };

/* GLSL.std.450 instruction numbers. */
enum {
    GlslTrunc = 3, GlslFAbs = 4, GlslFloor = 8, GlslCeil = 9, GlslFract = 10,
    GlslSin = 13, GlslCos = 14, GlslExp = 27, GlslExp2 = 29, GlslSqrt = 31,
    GlslInverseSqrt = 32, GlslFMin = 37, GlslFMax = 40, GlslFClamp = 43, GlslFMix = 46,
    GlslLength = 66, GlslCross = 68, GlslNormalize = 69
};

enum { EXT_TODO, EXT_OK, EXT_NO_LOG, EXT_INTEGER };
typedef struct { uint32_t num; const char *name; int nargs; int status; } ExtFn;

static const ExtFn g_extFns[] = {
    { 1, "round", 1, EXT_TODO },       { 2, "roundEven", 1, EXT_TODO },  { 3, "trunc", 1, EXT_OK },
    { 4, "abs", 1, EXT_OK },           { 5, "abs", 1, EXT_INTEGER },     { 6, "sign", 1, EXT_TODO },
    { 7, "sign", 1, EXT_INTEGER },     { 8, "floor", 1, EXT_OK },        { 9, "ceil", 1, EXT_OK },
    { 10, "fract", 1, EXT_OK },        { 11, "radians", 1, EXT_TODO },   { 12, "degrees", 1, EXT_TODO },
    { 13, "sin", 1, EXT_OK },          { 14, "cos", 1, EXT_OK },         { 15, "tan", 1, EXT_TODO },
    { 16, "asin", 1, EXT_TODO },       { 17, "acos", 1, EXT_TODO },      { 18, "atan", 1, EXT_TODO },
    { 19, "sinh", 1, EXT_TODO },       { 20, "cosh", 1, EXT_TODO },      { 21, "tanh", 1, EXT_TODO },
    { 25, "atan", 2, EXT_TODO },       { 26, "pow", 2, EXT_NO_LOG },     { 27, "exp", 1, EXT_OK },
    { 28, "log", 1, EXT_NO_LOG },      { 29, "exp2", 1, EXT_OK },        { 30, "log2", 1, EXT_NO_LOG },
    { 31, "sqrt", 1, EXT_OK },         { 32, "inversesqrt", 1, EXT_OK }, { 33, "determinant", 1, EXT_TODO },
    { 34, "inverse", 1, EXT_TODO },    { 35, "modf", 2, EXT_TODO },      { 37, "min", 2, EXT_OK },
    { 38, "min", 2, EXT_INTEGER },     { 39, "min", 2, EXT_INTEGER },    { 40, "max", 2, EXT_OK },
    { 41, "max", 2, EXT_INTEGER },     { 42, "max", 2, EXT_INTEGER },    { 43, "clamp", 3, EXT_OK },
    { 44, "clamp", 3, EXT_INTEGER },   { 45, "clamp", 3, EXT_INTEGER },  { 46, "mix", 3, EXT_OK },
    { 47, "mix", 3, EXT_INTEGER },     { 48, "step", 2, EXT_TODO },      { 49, "smoothstep", 3, EXT_TODO },
    { 50, "fma", 3, EXT_TODO },        { 66, "length", 1, EXT_OK },      { 67, "distance", 2, EXT_TODO },
    { 68, "cross", 2, EXT_OK },        { 69, "normalize", 1, EXT_OK },   { 70, "faceforward", 3, EXT_TODO },
    { 71, "reflect", 2, EXT_TODO },    { 72, "refract", 3, EXT_TODO },
};

static const ExtFn *ext_fn(uint32_t num)
{
    for (size_t i = 0; i < sizeof(g_extFns) / sizeof(g_extFns[0]); i++)
        if (g_extFns[i].num == num)
            return &g_extFns[i];
    return NULL;
}

/* What we know about each SPIR-V result id. */
typedef enum { ID_NONE, ID_TYPE, ID_CONST, ID_VAR, ID_VALUE, ID_PTR } IdKind;
typedef enum { T_OTHER, T_FLOAT, T_INT, T_VECTOR, T_MATRIX, T_STRUCT, T_POINTER, T_ARRAY,
               T_SAMPLER /* sampler2D: OpTypeSampledImage */, T_BOOL } TypeKind;

/* How a bool is known. VU1 has no bool registers: a comparison is made where it
 * is tested (the MAC flags of a subtraction), and/or/not combine such tests with
 * short-circuit branches, and a bool a phi carries across blocks is a float. */
enum { BOOL_NONE, BOOL_CMP, BOOL_FLOAT, BOOL_AND, BOOL_OR };

typedef struct {
    IdKind   kind;
    /* types */
    TypeKind type;
    uint32_t width;          /* float/int bits */
    uint32_t elemType;       /* vector component type, matrix column type, pointer pointee, array element */
    uint32_t elemCount;      /* vector size, matrix columns, array length */
    uint32_t stride;         /* array: bytes per element (ArrayStride) */
    uint32_t storage;        /* pointer storage class */
    const uint32_t *members; /* struct: member type ids; matrix constant: column constant ids */
    int      memberCount;
    /* constants and values */
    uint32_t value;
    uint32_t valType;        /* type id of a constant or value */
    uint32_t comps[4];       /* constant components (float bits) */
    int      compCount;      /* 0 if the constant isn't a float or float vector */
    int      vf00;           /* scalar constant 0 or 1: read from VF00 (field comp) */
    /* variables and pointers */
    uint32_t varType;        /* pointer type id */
    int      location;       /* -1 if none */
    int      binding;        /* -1 if none */
    int      set;            /* descriptor set, -1 if none */
    int      builtin;        /* -1 if none */
    int      structPosMember;/* gl_PerVertex: member index decorated Position */
    int      isPosition;     /* pointer that targets gl_Position */
    /* values: which input or constant qword a loaded value came from */
    int      fromInput;      /* index into inputs[], or -1 */
    int      fromConst;      /* constant qword: 0..PVK_VU1_PUSH_QWORDS-1 the push constants, then the
                                uniform area, then INSTANCE_QWORD; or -1 */
    int      instanceIndex;  /* the int value of gl_InstanceIndex, only float() may read it */
    /* indexing at run time */
    uint32_t dynIndex;       /* pointer: id of the index value computed at run time, 0 if none */
    int      dynStride;      /* pointer: qwords per element the index counts */
    int      dynCount;       /* pointer: elements (or columns) the index is clamped to */
    uint32_t floatIndex;     /* int value: the float it is int() of, 0 if none */
    uint32_t dynCol0;        /* matrix read at run time: id of its first column value, the rest follow */
    /* ints: VU1 computes with floats, so an int that is counted or compared is one */
    int      intIsFloat;     /* int value held as a float (a counter, a sum, a phi) */
    uint32_t asFloat;        /* int value: id of its float form once made, 0 if not yet */
    /* bools */
    int      boolKind;       /* BOOL_* */
    uint32_t boolA, boolB;   /* CMP: a - b is tested; FLOAT: the float; AND/OR: the two bools */
    int      boolZero;       /* CMP: test the zero flag (==) instead of the sign flag (<) */
    int      boolInv;        /* CMP/FLOAT: true when the flag is clear */
    /* values that live inside another value's register: v.w, v.xyz */
    uint32_t aliasOf;        /* id whose register this value shares, 0 if its own */
    int      comp;           /* field (0..3) holding a scalar value */
    int      mapOp;          /* 1 + body index of the shuffle/constructor that builds it, 0 if none */
    /* fragment shaders: what a value is, since the GS has fixed functions only */
    int      texel;          /* TEXEL_* */
    const char *name;
} IdInfo;

enum { TEXEL_NONE, TEXEL_SAMPLER, TEXEL_SAMPLE, TEXEL_SAMPLE_TIMES_COLOR };

typedef struct {
    int location;
    int size;                /* 4 (a color) or 2 (a texture coordinate) */
    uint32_t var;
} Var;

/* OpMemberDecorate on struct members (push constant blocks): byte Offset, and
 * for matrix members their MatrixStride and RowMajor/ColMajor layout. */
#define MAX_MEMBER_DECORS 256
typedef struct { uint32_t structType, member, offset, matrixStride; int hasOffset, rowMajor; } MemberDecor;
static MemberDecor g_memberDecors[MAX_MEMBER_DECORS];
static int g_memberDecorCount;

static const char *g_src = "shader";
static const char *g_sym = "";       /* symbol prefix of the microcode being written */
static int g_clamps = 0;             /* run-time index clamps labelled so far */
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
    case OpLoad: return "OpLoad (a push constant or uniform read)";
    case OpExtInst: return "OpExtInst (a GLSL built-in function)";
    case OpFunctionCall: return "OpFunctionCall (a call to another function)";
    case OpVectorShuffle: return "OpVectorShuffle (a swizzle)";
    case OpCompositeConstruct: return "OpCompositeConstruct (a vector constructor)";
    case OpCompositeExtract: return "OpCompositeExtract (a component read)";
    case OpSampledImage: case OpImageSampleImplicitLod: return "texture sampling";
    case OpImageSampleExplicitLod: return "textureLod() (a texture read at an explicit level of detail)";
    case OpFNegate: return "OpFNegate (negation)";
    case OpFAdd: return "OpFAdd (addition)";
    case OpFSub: return "OpFSub (subtraction)";
    case OpFMul: return "OpFMul (multiplication)";
    case OpFDiv: return "OpFDiv (division)";
    case OpVectorTimesScalar: return "OpVectorTimesScalar (vector * scalar)";
    case OpMatrixTimesVector: return "OpMatrixTimesVector (matrix * vector)";
    case OpVectorTimesMatrix: return "OpVectorTimesMatrix (vector * matrix: write it as matrix * vector)";
    case OpMatrixTimesMatrix: return "OpMatrixTimesMatrix (matrix * matrix: multiply once on the CPU and push the product)";
    case OpMatrixTimesScalar: return "OpMatrixTimesScalar (matrix * scalar: scale the matrix on the CPU)";
    case OpTranspose: return "OpTranspose (transpose: do it on the CPU)";
    case OpOuterProduct: return "OpOuterProduct (outer product)";
    case OpDot: return "OpDot (dot product)";
    case OpIAdd: case OpISub: case OpIMul: return "integer arithmetic (VU1 computes with floats only)";
    case OpConvertSToF: return "OpConvertSToF (float() of an integer)";
    case OpConvertFToS: return "OpConvertFToS (int() of a float)";
    case OpDynLoad: return "a block element picked at run time";
    case OpCompositeInsert: return "OpCompositeInsert (a component write)";
    case OpSelect: return "OpSelect (?:)";
    case OpPhi: return "OpPhi (a value chosen by a branch)";
    case OpBranch: case OpBranchConditional: case OpSelectionMerge: case OpLoopMerge:
        return "control flow (if/else or loops)";
    case OpSwitch: return "switch (write if/else)";
    case OpAny: case OpAll: return "any()/all() (compare components one at a time)";
    case OpUDiv: case OpSDiv: case OpUMod: case OpSRem: case OpSMod:
        return "integer division (VU1 computes with floats only: divide floats and take int() of the result)";
    case OpShiftRightLogical: case OpBitwiseXor: case OpNot: case OpBitcast:
        return "bit manipulation (VU1 computes with floats only)";
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
             "Only 'outColor = <color input>;', 'outColor = texture(<sampler>, <texcoord input>);' "
             "and their product are supported so far", what);
    fail("vertex shader uses %s, which this version of ps2vk-shaderc does not "
         "compile yet", what);
}

static uint32_t *read_spirv(const char *path, size_t *words);

/* Marks every loop for unrolling unless the GLSL says [[dont_unroll]]: spirv-opt
 * unrolls only loops that carry the flag, and only when it knows the count. */
static void mark_loops_unroll(const char *path)
{
    size_t words, pc;
    uint32_t *code = read_spirv(path, &words);
    FILE *f;

    for (pc = 5; pc < words;) {
        uint32_t op = code[pc] & 0xffff, n = code[pc] >> 16;
        if (n == 0 || pc + n > words)
            fail("corrupt SPIR-V from spirv-opt");
        if (op == OpLoopMerge && n >= 4 && !(code[pc + 3] & LoopControlDontUnroll))
            code[pc + 3] |= LoopControlUnroll;
        pc += n;
    }
    f = fopen(path, "wb");
    if (!f || fwrite(code, 4, words, f) != words)
        fail("cannot write %s", path);
    fclose(f);
    free(code);
}

/* Inlines calls, promotes locals to values and unrolls constant loops; the
 * result goes next to the output. */
static const char *optimize(const char *in, const char *outBase)
{
    static char opt[512];
    char cmd[1200];

    snprintf(opt, sizeof(opt), "%s.opt.spv", outBase);
    snprintf(cmd, sizeof(cmd),
             "spirv-opt --inline-entry-points-exhaustive --eliminate-dead-functions "
             "--convert-local-access-chains --scalar-replacement=100 --eliminate-local-single-block "
             "--eliminate-local-single-store --eliminate-local-multi-store "
             "--simplify-instructions --eliminate-dead-code-aggressive -o \"%s\" \"%s\"", opt, in);
    if (system(cmd) != 0)
        fail("spirv-opt failed on %s (SPIRV-Tools must be installed)", in);
    mark_loops_unroll(opt);
    snprintf(cmd, sizeof(cmd),
             "spirv-opt --loop-unroll --merge-blocks --simplify-instructions --ccp "
             "--eliminate-dead-code-aggressive --simplify-instructions -o \"%s\" \"%s\"", opt, opt);
    if (system(cmd) != 0)
        fail("spirv-opt failed on %s (SPIRV-Tools must be installed)", in);
    return opt;
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

static int is_float(const IdInfo *ids, uint32_t type)
{
    return ids[type].type == T_FLOAT && ids[type].width == 32;
}

static int is_int(const IdInfo *ids, uint32_t type)
{
    return ids[type].type == T_INT && ids[type].width == 32;
}

/* Component count of a float, int or vecN type, 0 for anything else. An int is
 * a float to VU1, exact below 2^24. */
static int type_size(const IdInfo *ids, uint32_t type)
{
    const IdInfo *t = &ids[type];
    if (is_float(ids, type) || is_int(ids, type))
        return 1;
    if (t->type == T_VECTOR && t->elemCount >= 2 && t->elemCount <= 4 && is_float(ids, t->elemType))
        return (int)t->elemCount;
    return 0;
}

/* Component count of an arithmetic operand or result: 1 (float) to 4 (vec4). */
static int value_size(const IdInfo *ids, uint32_t id, const char *stage)
{
    int size;

    if (ids[id].kind != ID_VALUE && !(ids[id].kind == ID_CONST && ids[id].compCount))
        fail("%s shader computes with something other than inputs and constants", stage);
    size = type_size(ids, ids[id].valType);
    if (!size)
        fail("%s shader computes with a type other than float, vec2, vec3 or vec4; only those are supported so far", stage);
    return size;
}

/* a < b and the like as VU1 tests them: the sign (S) or zero (Z) MAC flag of
 * a - b, or of b - a when swap is set, true when the flag is set unless inv. */
static int compare_kind(uint32_t op, int *swap, int *zero, int *inv)
{
    *swap = *zero = *inv = 0;
    switch (op) {
    case OpIEqual: case OpFOrdEqual: case OpFUnordEqual:
        *zero = 1; return 1;
    case OpINotEqual: case OpFOrdNotEqual: case OpFUnordNotEqual:
        *zero = 1; *inv = 1; return 1;
    case OpULessThan: case OpSLessThan: case OpFOrdLessThan: case OpFUnordLessThan:
        return 1;
    case OpUGreaterThan: case OpSGreaterThan: case OpFOrdGreaterThan: case OpFUnordGreaterThan:
        *swap = 1; return 1;
    case OpULessThanEqual: case OpSLessThanEqual: case OpFOrdLessThanEqual: case OpFUnordLessThanEqual:
        *swap = 1; *inv = 1; return 1;    /* !(b < a) */
    case OpUGreaterThanEqual: case OpSGreaterThanEqual: case OpFOrdGreaterThanEqual:
    case OpFUnordGreaterThanEqual:
        *inv = 1; return 1;               /* !(a < b) */
    default:
        return 0;
    }
}

/* The value that owns the register an alias (v.w, v.xyz) lives in. */
static uint32_t root(const IdInfo *ids, uint32_t id)
{
    while (ids[id].aliasOf)
        id = ids[id].aliasOf;
    return id;
}

/* Write mask for the first n fields: "x", "xy", "xyz", "xyzw". */
static const char *mask_n(int n)
{
    static const char *const masks[] = { "", "x", "xy", "xyz", "xyzw" };
    return masks[n];
}

/* ----------------------------------------------------------------------- */

/*
 * One instruction of a vertex shader body, in program order.
 *
 * Swizzles, constructors and component reads are all "field maps": each result
 * field names the operand and field it copies (or the constants 0 and 1, which
 * VF00 provides). Maps compose at parse time, so vec4(v.xyz, 1.0) is one map
 * over v however glslang split it up, and a map nobody but another map reads
 * emits no code.
 */
typedef struct {
    uint32_t op;
    uint32_t ext;                   /* OpExtInst: the GLSL.std.450 instruction */
    uint32_t result;
    uint32_t ops[MAX_OPERANDS];     /* operand ids: up to 4 columns and a vector */
    int      nops;
    int      fieldOp[4];            /* maps: operand index per result field, or FIELD_* */
    int      fieldComp[4];          /* maps: operand field per result field */
    int      dead;                  /* maps: result only feeds other maps, emit nothing */
    int      dynBase;               /* OpDynLoad: VU1 address the index is added to */
    int      dynStride;             /* OpDynLoad: qwords per element; 0 = VI08 still holds the address */
    int      dynInt;                /* OpDynLoad: ops[0] holds the index as an int (itof0 first), else a float */
    int      dynComp;               /* OpDynLoad: field of ops[0] holding the index */
    uint32_t dynLast;               /* OpDynLoad: the last valid index, float bits */
    uint32_t target, target2;       /* OpBranch(Conditional): labels taken when true, false */
    uint32_t cond;                  /* OpBranchConditional, OpSelect, OpBoolToFloat: the bool; its
                                       comparisons' operands follow the other operands in ops[] */
    int      outVar;                /* OpStore: OUT_* */
    uint32_t line;                  /* GLSL line, 0 if unknown */
} BodyOp;

enum { OUT_POSITION, OUT_COLOR, OUT_TEXCOORD, OUT_COUNT };
enum { FIELD_ZERO = -1, FIELD_ONE = -2, FIELD_UNDEF = -3 };
#define SHUFFLE_UNDEF 0xffffffffu

static int is_map_op(uint32_t op)
{
    return op == OpVectorShuffle || op == OpCompositeConstruct || op == OpCompositeExtract ||
           op == OpCompositeInsert ||
           op == OpLoad;   /* a vec2/vec3 push constant or uniform that does not start at field x */
}

static int is_branch_op(uint32_t op)
{
    return op == OpBranch || op == OpBranchConditional;
}

/* A phi: the value of its block is the source from whichever block ran last. */
typedef struct {
    uint32_t id;
    uint32_t vals[MAX_PHI_PREDS], preds[MAX_PHI_PREDS];
    int      count;
    int      isBool, isInt;
} Phi;

/* A uniform block: a descriptor set binding whose buffer the draw copies into
 * the VU1 uniform area, at the qword shaderc gives it. */
typedef struct {
    uint32_t    var;         /* the block variable */
    const char *name;
    int         set, binding;
    uint32_t    bytes;       /* size of the block, std140 */
    uint32_t    qword;       /* first qword in the uniform area */
} Block;

/* Constant qwords a shader may read: the push constants, then the uniform area. */
#define INSTANCE_QWORD (PVK_VU1_PUSH_QWORDS + PVK_VU1_UNIFORM_QWORDS) /* gl_InstanceIndex as a float in z */
#define CONST_QWORDS (INSTANCE_QWORD + 1)

typedef struct {
    uint32_t execModel;
    uint32_t glsl450;               /* id of the GLSL.std.450 import, 0 if none */
    Var      inputs[MAX_INPUTS];
    int      inputCount;
    int      colorInput;            /* fragment: index into inputs[] of the vec4 input, -1 if none */
    int      texcoordInput;         /* fragment: index into inputs[] of the vec2 input, -1 if none */
    uint32_t samplerVar;            /* fragment: the sampler2D variable, 0 if none */
    int      samplerBinding;        /* its binding, -1 if none */
    int      samplerSet;            /* its descriptor set, -1 if none */
    int      textureFunction;       /* fragment: GS TEX0.TFX, 0 modulate, 1 decal */
    uint32_t colorVar;              /* vertex: the vec4 output variable, 0 if none */
    uint32_t texcoordVar;           /* vertex: the vec2 output variable, 0 if none */
    int      texcoordLocation;      /* -1 if none */
    uint32_t texcoordValue;         /* value id stored to the texcoord output, 0 if never */
    uint32_t pushVar;               /* push constant block variable, 0 if none */
    uint32_t instanceIndexVar;      /* gl_InstanceIndex, 0 if none */
    uint32_t pushBytes;             /* size of the push constant block */
    Block    uniforms[PVK_MAX_UNIFORM_BLOCKS]; /* uniform blocks, sorted by (set, binding) */
    int      uniformCount;
    uint32_t constBase;             /* synthetic id of constant qword 0 (then qword 1, ...) */
    uint32_t nextId;                /* next free synthetic id, after the constant qwords */
    uint32_t floatType;             /* id of the 32-bit float type */
    uint32_t positionVar;           /* gl_PerVertex or Position variable, 0 if none */
    uint32_t positionValue;         /* value id stored to gl_Position, 0 if never */
    uint32_t colorValue;            /* value id stored to the color output, 0 if never */
    int      colorLocation;         /* -1 if none */
    BodyOp   body[MAX_BODY];
    int      bodyCount;
    Phi      phis[MAX_PHIS];
    int      phiCount;
    int      returns;               /* OpReturns seen; the body may have only one */
    uint32_t lastLabel;             /* the block OpReturn ends */
} Shader;

/* The next body op, zeroed, at the current GLSL line. */
static BodyOp *new_op(BodyOp *body, int *count, uint32_t op, uint32_t result)
{
    BodyOp *b;

    if (*count == MAX_BODY)
        fail("vertex shader is too long (max %d instructions)", MAX_BODY);
    b = &body[(*count)++];
    memset(b, 0, sizeof(*b));
    b->op = op;
    b->result = result;
    b->line = g_line;
    return b;
}

/* Where component j of value id really lives: (source id, field), seen through
 * aliases and earlier maps. */
typedef struct { uint32_t src; int comp; } Field;

/* The block an OpAccessChain starts from: the push constant block or one of
 * the uniform blocks. Gives the constant qword its byte 0 lives in and its
 * size; returns the kind of block for messages, or NULL. */
static const char *const_block(const Shader *sh, uint32_t var, int *qword, uint32_t *bytes)
{
    if (var && var == sh->pushVar) {
        *qword = 0;
        *bytes = sh->pushBytes;
        return "push constant";
    }
    for (int i = 0; i < sh->uniformCount; i++) {
        if (sh->uniforms[i].var == var) {
            *qword = PVK_VU1_PUSH_QWORDS + (int)sh->uniforms[i].qword;
            *bytes = sh->uniforms[i].bytes;
            return "uniform";
        }
    }
    return NULL;
}

static Field field_of(const IdInfo *ids, const Shader *sh, uint32_t id, int j)
{
    Field fl;

    if (ids[id].mapOp) {
        const BodyOp *m = &sh->body[ids[id].mapOp - 1];
        if (m->fieldOp[j] < 0) {
            fl.src = 0;
            fl.comp = m->fieldOp[j];
            return fl;
        }
        return field_of(ids, sh, m->ops[m->fieldOp[j]], m->fieldComp[j]);
    }
    if (ids[id].aliasOf)   /* comp is the field in the register; the root adds its own */
        return field_of(ids, sh, ids[id].aliasOf, ids[id].comp - ids[ids[id].aliasOf].comp + j);
    /* Every load of an input (or push constant qword) reads the same register:
     * name it by the variable so in.x and in.y loaded separately still group
     * into one MOVE. */
    if (ids[id].kind == ID_VALUE && ids[id].fromInput >= 0)
        fl.src = sh->inputs[ids[id].fromInput].var;
    else if (ids[id].kind == ID_VALUE && ids[id].fromConst >= 0)
        fl.src = sh->constBase + (uint32_t)ids[id].fromConst;
    else
        fl.src = id;
    fl.comp = ids[id].comp + j;
    return fl;
}

/* The decorations of a struct member; NULL if it has none and create is 0. */
static MemberDecor *member_decor(uint32_t structType, uint32_t member, int create)
{
    MemberDecor *d;

    for (int i = 0; i < g_memberDecorCount; i++)
        if (g_memberDecors[i].structType == structType && g_memberDecors[i].member == member)
            return &g_memberDecors[i];
    if (!create)
        return NULL;
    if (g_memberDecorCount == MAX_MEMBER_DECORS)
        fail("shader has too many block members (max %d)", MAX_MEMBER_DECORS);
    d = &g_memberDecors[g_memberDecorCount++];
    memset(d, 0, sizeof(*d));
    d->structType = structType;
    d->member = member;
    return d;
}

/* Byte offset of a struct member, from its Offset decoration. */
static uint32_t member_offset(uint32_t structType, uint32_t member)
{
    const MemberDecor *d = member_decor(structType, member, 0);

    if (!d || !d->hasOffset)
        fail("corrupt SPIR-V: block member %u has no Offset decoration", (unsigned)member);
    return d->offset;
}

/* Columns of a matCxR type (2..4), or 0 if the type is not a float matrix. */
static int matrix_cols(const IdInfo *ids, uint32_t type)
{
    const IdInfo *t = &ids[type];
    if (t->type != T_MATRIX || t->elemCount < 2 || t->elemCount > 4 || type_size(ids, t->elemType) < 2)
        return 0;
    return (int)t->elemCount;
}

/* Checks a push constant or uniform block (`kind` says which, for messages):
 * members are float, vec2, vec3, vec4, matrices or arrays of vectors, each in
 * whole qwords VU1 can load. Returns its size in bytes. */
static uint32_t check_block(const IdInfo *ids, uint32_t structType, const char *name, const char *kind)
{
    const IdInfo *s = &ids[structType];
    uint32_t bytes = 0;

    if (s->type != T_STRUCT)
        fail("%s '%s' is not a block; declare it as "
             "'layout(%s) uniform Name { ... } %s;'", kind, name,
             kind[0] == 'p' ? "push_constant" : "binding = 0", name);
    for (int m = 0; m < s->memberCount; m++) {
        uint32_t t = s->members[m], off = member_offset(structType, (uint32_t)m), end;
        uint32_t e = ids[t].type == T_ARRAY ? ids[t].elemType : t;
        if (ids[t].type == T_ARRAY && !type_size(ids, e) && !matrix_cols(ids, e))
            fail("member %d of %s block '%s' is an array of something other than "
                 "float, vec2, vec3, vec4 or a matrix, which is not supported yet", m, kind, name);
        if (matrix_cols(ids, e)) {
            /* VU1 reads a column per qword and broadcasts the vector's fields over
             * them, so the columns must be the qwords: column-major, 16 bytes apart
             * (what std430 and std140 give every matCxR). */
            const MemberDecor *d = member_decor(structType, (uint32_t)m, 0);
            if (d && d->rowMajor)
                fail("member %d of %s block '%s' is a row_major matrix; VU1 "
                     "multiplies by columns, so leave the block column-major (the default)",
                     m, kind, name);
            if (!d || d->matrixStride != 16)
                fail("corrupt SPIR-V: matrix member %d of %s block '%s' has a "
                     "column stride of %u bytes, not 16", m, kind, name, d ? (unsigned)d->matrixStride : 0u);
            if (off % 16)
                fail("member %d of %s block '%s' is a matrix at byte %u, which is "
                     "not 16-byte aligned", m, kind, name, (unsigned)off);
        }
        if (ids[t].type == T_ARRAY) {
            end = off + ids[t].stride * ids[t].elemCount;
        } else if (type_size(ids, t)) {
            end = off + 4 * (uint32_t)type_size(ids, t);
        } else if (matrix_cols(ids, t)) {
            end = off + 16 * (uint32_t)matrix_cols(ids, t);
        } else if (ids[t].type == T_INT && ids[t].width == 32) {
            end = off + 4;   /* may be declared; using it in arithmetic is rejected */
        } else {
            fail("member %d of %s block '%s' is something other than float, "
                 "vec2, vec3, vec4, a matrix, int or an array of vectors or matrices "
                 "(nested blocks are not supported yet)", m, kind, name);
        }
        if (end > bytes)
            bytes = end;
    }
    return bytes;
}

/* Gives the uniform blocks their places in the VU1 uniform area: one after
 * another in (set, binding) order, each starting on a qword. */
static void place_uniforms(Shader *sh)
{
    uint32_t qword = 0;

    for (int i = 0; i < sh->uniformCount; i++)
        for (int j = i + 1; j < sh->uniformCount; j++) {
            Block *a = &sh->uniforms[i], *b = &sh->uniforms[j];
            if (b->set < a->set || (b->set == a->set && b->binding < a->binding)) {
                Block t = *a; *a = *b; *b = t;
            }
        }
    for (int i = 0; i < sh->uniformCount; i++) {
        Block *b = &sh->uniforms[i];
        if (i && b->set == sh->uniforms[i - 1].set && b->binding == sh->uniforms[i - 1].binding)
            fail("uniform blocks '%s' and '%s' are both at set %d, binding %d",
                 sh->uniforms[i - 1].name, b->name, b->set, b->binding);
        b->qword = qword;
        qword += (b->bytes + 15) / 16;
    }
    if (qword > PVK_VU1_UNIFORM_QWORDS)
        fail("the uniform blocks take %u bytes together; ps2vk reserves %d bytes of VU1 memory "
             "for uniform buffers", (unsigned)qword * 16, PVK_VU1_UNIFORM_QWORDS * 16);
}

/* VU1 address of constant qword q (the push constants, then the uniform area). */
static int const_addr(int q)
{
    return q < PVK_VU1_PUSH_QWORDS ? PVK_VU1_PUSH_ADDR + q : PVK_VU1_UNIFORM_ADDR + q - PVK_VU1_PUSH_QWORDS;
}

/* Defines value id as qword `col` of the element pointer ptr picks at run
 * time: a body op that loads it through VI08. The first column computes the
 * address from the index (an int block member, gl_InstanceIndex or a float,
 * clamped to the array then truncated with FTOI0), the others reuse it. */
static uint32_t int_operand(IdInfo *ids, Shader *sh, uint32_t id, BodyOp *body, int *count);

static void define_dyn_load(IdInfo *ids, Shader *sh, uint32_t id, uint32_t type, uint32_t ptr, int col)
{
    uint32_t dynIndex = ids[ptr].dynIndex;
    const IdInfo *idx = &ids[dynIndex];
    float last = (float)(ids[ptr].dynCount - 1);
    uint32_t src = 0;
    int comp = 0, isInt = 0;
    BodyOp *b;

    if (!col) {
        if (idx->floatIndex) {
            src = idx->floatIndex;
            comp = ids[src].comp;
        } else if (idx->instanceIndex) {
            src = sh->constBase + INSTANCE_QWORD;
            comp = 2;
        } else if (idx->fromConst >= 0 && !idx->intIsFloat) {
            src = sh->constBase + (uint32_t)idx->fromConst;
            comp = idx->comp;
            isInt = 1;
        } else {
            src = int_operand(ids, sh, dynIndex, sh->body, &sh->bodyCount);
            comp = ids[src].comp;
        }
    }
    ids[id].kind = ID_VALUE;
    ids[id].valType = type;
    b = new_op(sh->body, &sh->bodyCount, OpDynLoad, id);
    b->dynBase = const_addr(ids[ptr].fromConst) + col;
    b->dynStride = col ? 0 : ids[ptr].dynStride;
    memcpy(&b->dynLast, &last, sizeof(last));
    if (col)
        return;
    b->ops[b->nops++] = src;
    b->dynComp = comp;
    b->dynInt = isInt;
}

/* A fresh id with no SPIR-V counterpart. */
static uint32_t new_id(Shader *sh)
{
    if (sh->nextId == MAX_IDS)
        fail("shader is too large (%u ids)", (unsigned)sh->nextId);
    return sh->nextId++;
}

/* Defines id as the same value as src (float(i) of an int VU1 already holds as a float). */
static void alias_value(IdInfo *ids, uint32_t id, uint32_t type, uint32_t src)
{
    ids[id].kind = ID_VALUE;
    ids[id].valType = type;
    ids[id].comp = ids[src].comp;
    if (ids[src].fromConst >= 0 || ids[src].fromInput >= 0) {
        ids[id].fromConst = ids[src].fromConst;
        ids[id].fromInput = ids[src].fromInput;
    } else {
        ids[id].aliasOf = root(ids, src);
    }
}

/* An int as the float VU1 computes with: a counter or sum as is, int() of a
 * float truncated, an int read from a block through ITOF0 (appended to body),
 * a constant as a float constant. */
static uint32_t int_operand(IdInfo *ids, Shader *sh, uint32_t id, BodyOp *body, int *count)
{
    uint32_t n;
    BodyOp *b;

    if (ids[id].kind == ID_CONST || !is_int(ids, ids[id].valType) || ids[id].intIsFloat)
        return id;
    if (ids[id].kind != ID_VALUE)
        fail("vertex shader computes with something other than inputs and constants");
    if (ids[id].asFloat)
        return ids[id].asFloat;
    n = new_id(sh);
    ids[n].kind = ID_VALUE;
    ids[n].valType = sh->floatType;
    if (ids[id].instanceIndex) {
        ids[n].fromConst = INSTANCE_QWORD;
        ids[n].comp = 2;
    } else if (ids[id].floatIndex) {
        b = new_op(body, count, OpExtInst, n);
        b->ext = GlslTrunc;
        b->ops[b->nops++] = ids[id].floatIndex;
        ids[n].comp = ids[ids[id].floatIndex].comp;
    } else {
        b = new_op(body, count, OpConvertSToF, n);
        b->ops[b->nops++] = id;
        ids[n].comp = ids[id].comp;
    }
    ids[id].asFloat = n;
    return n;
}

/* Defines id as !src: a flipped test, or De Morgan over and/or. */
static void negate_bool(IdInfo *ids, Shader *sh, uint32_t id, uint32_t type, uint32_t src)
{
    ids[id].kind = ID_VALUE;
    ids[id].valType = type;
    ids[id].boolKind = ids[src].boolKind;
    ids[id].boolA = ids[src].boolA;
    ids[id].boolB = ids[src].boolB;
    ids[id].boolZero = ids[src].boolZero;
    ids[id].boolInv = !ids[src].boolInv;
    if (ids[src].boolKind == BOOL_AND || ids[src].boolKind == BOOL_OR) {
        ids[id].boolKind = ids[src].boolKind == BOOL_AND ? BOOL_OR : BOOL_AND;
        ids[id].boolA = new_id(sh);
        ids[id].boolB = new_id(sh);
        negate_bool(ids, sh, ids[id].boolA, type, ids[src].boolA);
        negate_bool(ids, sh, ids[id].boolB, type, ids[src].boolB);
    }
}

/* Appends the values a bool's tests read to b->ops, in the order emit_cond reads them. */
static void collect_leaves(const IdInfo *ids, uint32_t cond, BodyOp *b)
{
    switch (ids[cond].boolKind) {
    case BOOL_CMP: case BOOL_FLOAT:
        if (b->nops + 2 > MAX_OPERANDS)
            fail("condition has too many comparisons (max %d); split it into nested ifs",
                 (MAX_OPERANDS - 2) / 2);
        b->ops[b->nops++] = ids[cond].boolA;
        if (ids[cond].boolKind == BOOL_CMP)
            b->ops[b->nops++] = ids[cond].boolB;
        break;
    case BOOL_AND: case BOOL_OR:
        collect_leaves(ids, ids[cond].boolA, b);
        collect_leaves(ids, ids[cond].boolB, b);
        break;
    default:
        fail("vertex shader tests a bool that is not a comparison (a true/false constant, or a "
             "bool chosen by ?:), which this version of ps2vk-shaderc does not compile");
    }
}

/* Defines value id (a float or vecN of `size` fields) as the given fields: a
 * constant when every field is one, an alias when they are the leading fields of
 * one value, else a map body op. */
static void define_map(IdInfo *ids, Shader *sh, uint32_t op, uint32_t id, uint32_t type,
                       const Field *fl, int size)
{
    int allConst = 1, aliasable = 1;
    BodyOp *b;

    ids[id].valType = type;
    for (int j = 0; j < size; j++) {
        if (fl[j].src && ids[fl[j].src].kind != ID_CONST)
            allConst = 0;
        if (fl[j].src == 0 && fl[j].comp != FIELD_UNDEF)
            allConst = 0;
        if (fl[j].src != fl[0].src || fl[j].src == 0 || fl[j].comp != fl[0].comp + j)
            aliasable = 0;
    }
    if (allConst) {
        ids[id].kind = ID_CONST;
        for (int j = 0; j < size; j++)
            ids[id].comps[j] = fl[j].src ? ids[fl[j].src].comps[fl[j].comp] : 0;
        ids[id].compCount = size;
        return;
    }
    ids[id].kind = ID_VALUE;
    if (aliasable && (size == 1 || fl[0].comp == 0)) {
        /* v.w or v.xyz: read in place, no code. */
        ids[id].aliasOf = fl[0].src;
        ids[id].comp = fl[0].comp;
        return;
    }
    if (sh->bodyCount == MAX_BODY)
        fail("vertex shader is too long (max %d instructions)", MAX_BODY);
    b = &sh->body[sh->bodyCount++];
    b->op = op;
    b->result = id;
    b->line = g_line;
    for (int j = 0; j < size; j++) {
        uint32_t src = fl[j].src;
        int k;
        if (!src) {
            b->fieldOp[j] = fl[j].comp;
            continue;
        }
        if (ids[src].kind == ID_CONST) {
            uint32_t bits = ids[src].comps[fl[j].comp];
            if (!(bits & 0x7fffffff)) { b->fieldOp[j] = FIELD_ZERO; continue; }
            if (bits == 0x3f800000) { b->fieldOp[j] = FIELD_ONE; continue; }
        }
        for (k = 0; k < b->nops && b->ops[k] != src; k++)
            ;
        if (k == b->nops)
            b->ops[b->nops++] = src;
        b->fieldOp[j] = k;
        b->fieldComp[j] = fl[j].comp;
    }
    ids[id].mapOp = sh->bodyCount;
}

static uint32_t canon(const IdInfo *ids, const Shader *sh, uint32_t id);

/* Maps are found by body index: recompute after the body is rebuilt. */
static void renumber_maps(IdInfo *ids, Shader *sh)
{
    for (int i = 0; i < sh->bodyCount; i++)
        if (is_map_op(sh->body[i].op))
            ids[sh->body[i].result].mapOp = i + 1;
}

/*
 * Replaces the phis and the stores to outputs by moves: each predecessor copies
 * its value into the phi's register right before it branches, and an output
 * stored on several paths gets a register of its own that every store moves
 * into (an output stored once stays a plain value). A bool source becomes a
 * float (0 = false) and an int source the float VU1 counts with.
 */
static void rebuild_phis(IdInfo *ids, Shader *sh)
{
    static BodyOp out[MAX_BODY];
    uint32_t *outValue[OUT_COUNT] = { &sh->positionValue, &sh->colorValue, &sh->texcoordValue };
    uint32_t synth[OUT_COUNT] = { 0 }, label = 0;
    int n = 0, stores[OUT_COUNT] = { 0 };

    for (int i = 0; i < sh->bodyCount; i++)
        if (sh->body[i].op == OpStore)
            stores[sh->body[i].outVar]++;

    for (int i = 0; i < sh->bodyCount; i++) {
        BodyOp *b = &sh->body[i];

        g_line = b->line;
        if (b->op == OpLabel) {
            label = b->result;
        } else if (b->op == OpStore) {
            uint32_t val = b->ops[0];
            int o = b->outVar;
            if (stores[o] == 1) {
                *outValue[o] = val;
                continue;
            }
            if (!synth[o]) {
                synth[o] = new_id(sh);
                ids[synth[o]].kind = ID_VALUE;
                ids[synth[o]].valType = ids[val].valType;
                *outValue[o] = synth[o];
            }
            b = new_op(out, &n, OpPhiMove, synth[o]);
            b->ops[b->nops++] = val;
            continue;
        } else if (is_branch_op(b->op)) {
            for (int p = 0; p < sh->phiCount; p++) {
                const Phi *ph = &sh->phis[p];
                for (int k = 0; k < ph->count; k++) {
                    uint32_t src = ph->vals[k];
                    BodyOp *m;
                    if (ph->preds[k] != label)
                        continue;
                    if (ph->isBool) {
                        if (ids[src].boolKind == BOOL_FLOAT) {
                            src = ids[src].boolA;
                        } else {
                            uint32_t fl = new_id(sh);
                            ids[fl].kind = ID_VALUE;
                            ids[fl].valType = sh->floatType;
                            m = new_op(out, &n, OpBoolToFloat, fl);
                            m->cond = src;
                            collect_leaves(ids, src, m);
                            src = fl;
                        }
                    } else if (ph->isInt) {
                        src = int_operand(ids, sh, src, out, &n);
                    }
                    if (ids[src].kind != ID_VALUE && !(ids[src].kind == ID_CONST && ids[src].compCount))
                        fail("corrupt SPIR-V: a phi of something other than a value");
                    m = new_op(out, &n, OpPhiMove, ph->id);
                    m->ops[m->nops++] = src;
                }
            }
        }
        if (n == MAX_BODY)
            fail("vertex shader is too long (max %d instructions)", MAX_BODY);
        out[n++] = *b;
    }
    g_line = 0;
    memcpy(sh->body, out, (size_t)n * sizeof(out[0]));
    sh->bodyCount = n;
    renumber_maps(ids, sh);
}

/*
 * A push constant or uniform qword is loaded (one LQ) in every block that reads
 * it, right before the first read there, and its register is given back after
 * the last read in that block: a reload is cheaper than a register held across
 * a branch or a loop. The reads are repointed at the block's loaded value.
 */
static void place_const_loads(IdInfo *ids, Shader *sh)
{
    static BodyOp out[MAX_BODY];
    uint32_t loaded[CONST_QWORDS];   /* in the current block: value id holding qword q, 0 if none */
    uint32_t *outputs[3] = { &sh->positionValue, &sh->colorValue, &sh->texcoordValue };
    int n = 0;

    memset(loaded, 0, sizeof(loaded));
    for (int i = 0; i <= sh->bodyCount; i++) {
        BodyOp *b = i < sh->bodyCount ? &sh->body[i] : NULL;
        int nops = b ? b->nops : 3;

        if (b && b->op == OpLabel)
            memset(loaded, 0, sizeof(loaded));
        for (int j = 0; j < nops && !(b && b->dead); j++) {
            uint32_t *op = b ? &b->ops[j] : outputs[j], c, a;
            int q;
            c = *op ? canon(ids, sh, *op) : 0;
            if (c < sh->constBase || c >= sh->constBase + CONST_QWORDS)
                continue;
            q = (int)(c - sh->constBase);
            if (!loaded[q]) {
                BodyOp *l;
                loaded[q] = new_id(sh);
                ids[loaded[q]].kind = ID_VALUE;
                ids[loaded[q]].valType = sh->floatType;
                if (n == MAX_BODY)
                    fail("vertex shader is too long (max %d instructions)", MAX_BODY);
                l = &out[n++];
                memset(l, 0, sizeof(*l));
                l->op = OpConstLoad;
                l->result = loaded[q];
                l->target = (uint32_t)q;
            }
            if (ids[*op].comp == 0) {
                *op = loaded[q];
                continue;
            }
            a = new_id(sh);   /* pc.v.w: the loaded qword's field */
            ids[a].kind = ID_VALUE;
            ids[a].valType = ids[*op].valType;
            ids[a].comp = ids[*op].comp;
            ids[a].aliasOf = loaded[q];
            *op = a;
        }
        if (b) {
            if (n == MAX_BODY)
                fail("vertex shader is too long (max %d instructions)", MAX_BODY);
            out[n++] = *b;
        }
    }
    memcpy(sh->body, out, (size_t)n * sizeof(out[0]));
    sh->bodyCount = n;
    renumber_maps(ids, sh);
}

static void parse(const uint32_t *code, size_t words, IdInfo *ids, Shader *sh)
{
    size_t pc;
    uint32_t bound = code[3];
    int inFunction = 0, functions = 0;

    /* Ids past the SPIR-V bound stand for the push constant and uniform qwords. */
    if (bound + CONST_QWORDS > MAX_IDS)
        fail("shader is too large (%u ids)", (unsigned)bound);

    memset(sh, 0, sizeof(*sh));
    sh->execModel = ~0u;
    sh->colorLocation = sh->texcoordLocation = -1;
    sh->colorInput = sh->texcoordInput = sh->samplerBinding = sh->samplerSet = -1;
    sh->constBase = bound;
    sh->nextId = bound + CONST_QWORDS;
    for (uint32_t i = 0; i < MAX_IDS; i++) {
        ids[i].location = ids[i].binding = ids[i].set = ids[i].builtin = ids[i].structPosMember = -1;
        ids[i].fromInput = ids[i].fromConst = -1;
    }
    for (int q = 0; q < CONST_QWORDS; q++) {
        ids[sh->constBase + q].kind = ID_VALUE;
        ids[sh->constBase + q].fromConst = q;
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
        case OpExtInstImport:
            if (strcmp((const char *)&w[2], "GLSL.std.450"))
                fail("shader imports extended instruction set '%s', which is not supported",
                     (const char *)&w[2]);
            sh->glsl450 = w[1];
            break;
        case OpName:
            ids[w[1]].name = (const char *)&w[2];
            break;
        case OpDecorate:
            if (w[2] == DecLocation)
                ids[w[1]].location = (int)w[3];
            else if (w[2] == DecBinding)
                ids[w[1]].binding = (int)w[3];
            else if (w[2] == DecDescriptorSet)
                ids[w[1]].set = (int)w[3];
            else if (w[2] == DecBuiltIn)
                ids[w[1]].builtin = (int)w[3];
            else if (w[2] == DecArrayStride)
                ids[w[1]].stride = w[3];
            break;
        case OpMemberDecorate:
            if (w[3] == DecBuiltIn && w[4] == BuiltInPosition) {
                ids[w[1]].structPosMember = (int)w[2];
            } else if (w[3] == DecOffset) {
                MemberDecor *d = member_decor(w[1], w[2], 1);
                d->offset = w[4];
                d->hasOffset = 1;
            } else if (w[3] == DecMatrixStride) {
                member_decor(w[1], w[2], 1)->matrixStride = w[4];
            } else if (w[3] == DecRowMajor) {
                member_decor(w[1], w[2], 1)->rowMajor = 1;
            }
            break;
        case OpTypeFloat:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_FLOAT; ids[w[1]].width = w[2];
            if (w[2] == 32 && !sh->floatType)
                sh->floatType = w[1];
            break;
        case OpTypeInt:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_INT; ids[w[1]].width = w[2];
            ids[w[1]].storage = w[3];   /* signedness */
            break;
        case OpTypeBool:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_BOOL;
            break;
        case OpTypeVector:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_VECTOR;
            ids[w[1]].elemType = w[2]; ids[w[1]].elemCount = w[3];
            break;
        case OpTypeStruct:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_STRUCT;
            ids[w[1]].members = &w[2]; ids[w[1]].memberCount = (int)(n - 2);
            break;
        case OpTypeArray:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_ARRAY;
            ids[w[1]].elemType = w[2];
            ids[w[1]].elemCount = ids[w[3]].kind == ID_CONST ? ids[w[3]].value : 0;
            break;
        case OpTypePointer:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_POINTER;
            ids[w[1]].storage = w[2]; ids[w[1]].elemType = w[3];
            break;
        case OpTypeMatrix:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_MATRIX;
            ids[w[1]].elemType = w[2]; ids[w[1]].elemCount = w[3];
            break;
        case OpTypeSampledImage:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_SAMPLER;
            break;
        case OpTypeImage: case OpTypeSampler:
        case OpTypeVoid: case OpTypeRuntimeArray:
        case OpTypeFunction:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_OTHER;
            break;
        case OpConstant:
            ids[w[2]].kind = ID_CONST; ids[w[2]].value = w[3];
            ids[w[2]].valType = w[1];
            if (is_float(ids, w[1])) {
                ids[w[2]].comps[0] = w[3];
                ids[w[2]].compCount = 1;
            } else if (is_int(ids, w[1])) {
                /* Also the float VU1 counts and compares with. */
                float v = ids[w[1]].storage ? (float)(int32_t)w[3] : (float)w[3];
                memcpy(&ids[w[2]].comps[0], &v, sizeof(v));
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
            } else if (ids[w[1]].type == T_MATRIX) {
                /* A constant matrix: its columns are vector constants of their own. */
                ids[id].members = &w[3];
                ids[id].memberCount = (int)(n - 3);
            }
            break;
        }
        case OpVariable: {
            uint32_t type = w[1], id = w[2], storage = w[3];
            ids[id].kind = ID_VAR;
            ids[id].varType = type;
            if (!inFunction && storage == ScPushConstant && sh->execModel == ExecFragment)
                fail("fragment shader reads push constant '%s': the GS cannot read per-draw "
                     "values. Push constants are only available in vertex shaders",
                     ids[id].name ? ids[id].name : "variable");
            if (!inFunction && storage == ScUniform && sh->execModel == ExecFragment)
                fail("fragment shader reads uniform block '%s': the GS cannot read per-draw "
                     "values. Uniform buffers are only available in vertex shaders",
                     ids[id].name ? ids[id].name : "variable");
            if (!inFunction && storage == ScUniformConstant && ids[ids[type].elemType].type == T_SAMPLER) {
                if (sh->execModel != ExecFragment)
                    fail("vertex shader declares sampler '%s': VU1 cannot read textures. "
                         "Sample in the fragment shader instead",
                         ids[id].name ? ids[id].name : "variable");
            } else if (!inFunction && storage == ScUniformConstant) {
                fail("'%s' is a separate texture or sampler; the GS binds both together, "
                     "so declare a sampler2D", ids[id].name ? ids[id].name : "variable");
            } else if (!inFunction && storage != ScInput && storage != ScOutput && storage != ScPushConstant
                       && storage != ScUniform) {
                fail("'%s' is a storage buffer or something else this version of ps2vk-shaderc "
                     "does not support; it supports shader inputs, outputs, push constants, "
                     "uniform blocks and a sampler2D", ids[id].name ? ids[id].name : "variable");
            }
            if (inFunction && storage == ScFunction)
                fail("local variable '%s' is still in memory after spirv-opt: VU1 keeps locals "
                     "in registers, so index local arrays with constants and keep locals "
                     "to vectors, matrices and structs of them",
                     ids[id].name ? ids[id].name : "variable");
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
    if (!sh->floatType)
        fail("shader has no float type");
    if (functions != 1)
        fail("shader has %d functions after spirv-opt inlined its calls; a recursive function "
             "cannot be inlined", functions);

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

            if (storage == ScInput && ids[id].builtin == BuiltInInstanceIndex && sh->execModel == ExecVertex) {
                sh->instanceIndexVar = id;
            } else if (storage == ScInput) {
                int size = type_size(ids, pointee);

                if (ids[id].builtin >= 0)
                    fail("built-in input '%s' is not supported", name);
                if (ids[id].location < 0)
                    fail("input '%s' has no location", name);
                if (sh->execModel == ExecVertex) {
                    if (size != 4)
                        fail("input '%s' must be a vec4 for now", name);
                } else if (size == 4) {
                    if (sh->colorInput >= 0)
                        fail("fragment shader has two vec4 inputs; the GS interpolates only one color");
                    sh->colorInput = sh->inputCount;
                } else if (size == 2) {
                    if (sh->texcoordInput >= 0)
                        fail("fragment shader has two vec2 inputs; the GS interpolates only one "
                             "texture coordinate pair");
                    sh->texcoordInput = sh->inputCount;
                } else {
                    fail("fragment input '%s' must be a vec4 (the color) or a vec2 (the texture "
                         "coordinate); the GS interpolates nothing else", name);
                }
                if (sh->inputCount == MAX_INPUTS)
                    fail("too many inputs (max %d)", MAX_INPUTS);
                sh->inputs[sh->inputCount].location = ids[id].location;
                sh->inputs[sh->inputCount].size = size;
                sh->inputs[sh->inputCount].var = id;
                sh->inputCount++;
            } else if (storage == ScUniformConstant) {
                if (sh->samplerVar)
                    fail("fragment shader declares two samplers; the GS reads one texture per draw");
                if (ids[id].binding < 0)
                    fail("sampler '%s' has no binding", name);
                sh->samplerVar = id;
                sh->samplerBinding = ids[id].binding;
                sh->samplerSet = ids[id].set < 0 ? 0 : ids[id].set; /* no layout(set) means set 0 */
            } else if (storage == ScPushConstant) {
                if (sh->pushVar)
                    fail("shader has two push constant blocks; only one is allowed");
                sh->pushVar = id;
                sh->pushBytes = check_block(ids, pointee, name, "push constant");
                if (sh->pushBytes > PVK_VU1_PUSH_QWORDS * 16)
                    fail("push constant block '%s' is %u bytes; ps2vk reserves %d bytes of VU1 memory "
                         "for push constants", name, (unsigned)sh->pushBytes, PVK_VU1_PUSH_QWORDS * 16);
            } else if (storage == ScUniform) {
                Block *b;

                if (sh->uniformCount == PVK_MAX_UNIFORM_BLOCKS)
                    fail("shader has more than %d uniform blocks", PVK_MAX_UNIFORM_BLOCKS);
                if (ids[id].binding < 0)
                    fail("uniform block '%s' has no binding; give it layout(set = s, binding = b)", name);
                b = &sh->uniforms[sh->uniformCount++];
                b->var = id;
                b->name = name;
                b->set = ids[id].set < 0 ? 0 : ids[id].set; /* no layout(set) means set 0 */
                b->binding = ids[id].binding;
                b->bytes = check_block(ids, pointee, name, "uniform");
                if (b->bytes > PVK_VU1_UNIFORM_QWORDS * 16)
                    fail("uniform block '%s' is %u bytes; ps2vk reserves %d bytes of VU1 memory "
                         "for uniform buffers", name, (unsigned)b->bytes, PVK_VU1_UNIFORM_QWORDS * 16);
            } else if (storage == ScOutput) {
                if (ids[pointee].type == T_STRUCT && ids[pointee].structPosMember >= 0) {
                    sh->positionVar = id; /* gl_PerVertex block */
                } else if (ids[id].builtin == BuiltInPosition) {
                    sh->positionVar = id;
                    ids[id].isPosition = 1;
                } else if (ids[id].builtin >= 0) {
                    fail("built-in output '%s' is not supported", name);
                } else {
                    int size = type_size(ids, pointee);

                    if (ids[id].location < 0)
                        fail("output '%s' has no location", name);
                    if (size == 4) {
                        if (sh->colorVar)
                            fail("shader has 2 color outputs; the GS can interpolate only one color");
                        sh->colorVar = id;
                        sh->colorLocation = ids[id].location;
                    } else if (size == 2 && sh->execModel == ExecVertex) {
                        if (sh->texcoordVar)
                            fail("vertex shader has 2 texture coordinate outputs; the GS "
                                 "interpolates only one pair (S, T)");
                        sh->texcoordVar = id;
                        sh->texcoordLocation = ids[id].location;
                    } else if (sh->execModel == ExecVertex) {
                        fail("vertex output '%s' must be a vec4 (the color) or a vec2 (the "
                             "texture coordinate); the GS interpolates nothing else", name);
                    } else {
                        fail("fragment output '%s' must be a vec4", name);
                    }
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
    for (int i = 0; i < sh->inputCount; i++) {
        ids[sh->inputs[i].var].fromInput = i;
        if (sh->execModel == ExecFragment) {
            if (sh->inputs[i].size == 4)
                sh->colorInput = i;
            else
                sh->texcoordInput = i;
        }
    }

    if (sh->execModel == ExecFragment) {
        if (!sh->colorVar || sh->colorLocation != 0)
            fail("fragment shader must have exactly one vec4 output at location 0");
        if (sh->samplerVar && sh->texcoordInput < 0)
            fail("fragment shader declares a sampler but has no vec2 input to sample it with");
    }
    place_uniforms(sh);

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
            case OpLine: case OpNoLine: case OpNop: case OpFunctionEnd:
            case OpSelectionMerge: case OpLoopMerge: case OpUnreachable:
                break;
            case OpLabel:
                new_op(sh->body, &sh->bodyCount, OpLabel, w[1]);
                sh->lastLabel = w[1];
                break;
            case OpReturn:
                sh->returns++;
                break;
            case OpBranch: {
                BodyOp *b = new_op(sh->body, &sh->bodyCount, OpBranch, 0);
                b->target = w[1];
                break;
            }
            case OpBranchConditional: {
                BodyOp *b;
                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                b = new_op(sh->body, &sh->bodyCount, OpBranchConditional, 0);
                b->cond = w[1];
                b->target = w[2];
                b->target2 = w[3];
                collect_leaves(ids, w[1], b);
                break;
            }
            case OpPhi: {
                /* The value becomes a register every predecessor writes before
                 * branching here (rebuild_phis). */
                uint32_t id = w[2], type = w[1];
                Phi *p;
                int size = type_size(ids, type);

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                if (sh->phiCount == MAX_PHIS)
                    fail("vertex shader has more than %d values chosen by branches", MAX_PHIS);
                if (!size && ids[type].type != T_BOOL)
                    fail("vertex shader keeps a %s across a branch or loop; only floats, ints, "
                         "vectors and bools can be",
                         ids[type].type == T_MATRIX ? "matrix" : "struct or array");
                p = &sh->phis[sh->phiCount++];
                memset(p, 0, sizeof(*p));
                p->id = id;
                p->isBool = ids[type].type == T_BOOL;
                p->isInt = is_int(ids, type);
                for (uint32_t i = 3; i + 1 < n; i += 2) {
                    if (p->count == MAX_PHI_PREDS)
                        fail("corrupt SPIR-V: a phi with more than %d predecessors", MAX_PHI_PREDS);
                    p->vals[p->count] = w[i];
                    p->preds[p->count] = w[i + 1];
                    p->count++;
                }
                ids[id].kind = ID_VALUE;
                ids[id].valType = p->isBool ? sh->floatType : type;
                ids[id].comp = 0;
                ids[id].intIsFloat = p->isInt;
                if (p->isBool) {
                    ids[id].boolKind = BOOL_FLOAT;
                    ids[id].boolA = id;
                }
                break;
            }
            case OpIEqual: case OpINotEqual: case OpUGreaterThan: case OpSGreaterThan:
            case OpUGreaterThanEqual: case OpSGreaterThanEqual: case OpULessThan: case OpSLessThan:
            case OpULessThanEqual: case OpSLessThanEqual: case OpFOrdEqual: case OpFUnordEqual:
            case OpFOrdNotEqual: case OpFUnordNotEqual: case OpFOrdLessThan: case OpFUnordLessThan:
            case OpFOrdGreaterThan: case OpFUnordGreaterThan: case OpFOrdLessThanEqual:
            case OpFUnordLessThanEqual: case OpFOrdGreaterThanEqual: case OpFUnordGreaterThanEqual: {
                uint32_t id = w[2], a, c;
                int swap, zero, inv;

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                compare_kind(op, &swap, &zero, &inv);
                if (ids[w[1]].type != T_BOOL)
                    fail("vertex shader compares vectors; VU1 tests one value at a time, so "
                         "compare components one by one");
                a = int_operand(ids, sh, w[3], sh->body, &sh->bodyCount);
                c = int_operand(ids, sh, w[4], sh->body, &sh->bodyCount);
                if (value_size(ids, a, stage) != 1 || value_size(ids, c, stage) != 1)
                    fail("corrupt SPIR-V: comparison of non-scalars with a bool result");
                ids[id].kind = ID_VALUE;
                ids[id].valType = w[1];
                ids[id].boolKind = BOOL_CMP;
                ids[id].boolA = swap ? c : a;
                ids[id].boolB = swap ? a : c;
                ids[id].boolZero = zero;
                ids[id].boolInv = inv;
                break;
            }
            case OpLogicalAnd: case OpLogicalOr: {
                uint32_t id = w[2];
                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                if (!ids[w[3]].boolKind || !ids[w[4]].boolKind)
                    fail("vertex shader combines bools that are not comparisons with %s",
                         op == OpLogicalAnd ? "&&" : "||");
                ids[id].kind = ID_VALUE;
                ids[id].valType = w[1];
                ids[id].boolKind = op == OpLogicalAnd ? BOOL_AND : BOOL_OR;
                ids[id].boolA = w[3];
                ids[id].boolB = w[4];
                break;
            }
            case OpLogicalNot:
                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                if (!ids[w[3]].boolKind)
                    fail("vertex shader negates a bool that is not a comparison");
                negate_bool(ids, sh, w[2], w[1], w[3]);
                break;
            case OpSelect: {
                /* c ? a : b, as a conditional move. */
                uint32_t id = w[2], a, c;
                BodyOp *b;
                int size;

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                if (ids[w[1]].type == T_BOOL)
                    fail("vertex shader chooses between bools with ?:; VU1 has no bool registers, "
                         "so choose between floats (0.0 and 1.0) or restructure the condition");
                if (!ids[w[3]].boolKind)
                    fail("vertex shader uses ?: on a bool that is not a comparison");
                size = type_size(ids, w[1]);
                if (!size)
                    fail("vertex shader chooses a %s with ?:; only floats, ints and vectors can be",
                         ids[w[1]].type == T_MATRIX ? "matrix" : "struct, array or bool vector");
                a = int_operand(ids, sh, w[4], sh->body, &sh->bodyCount);
                c = int_operand(ids, sh, w[5], sh->body, &sh->bodyCount);
                if (value_size(ids, a, stage) != size || value_size(ids, c, stage) != size)
                    fail("corrupt SPIR-V: OpSelect operands differ in size from the result");
                ids[id].kind = ID_VALUE;
                ids[id].valType = w[1];
                ids[id].comp = size == 1 ? ids[a].comp : 0;
                ids[id].intIsFloat = is_int(ids, w[1]);
                b = new_op(sh->body, &sh->bodyCount, OpSelect, id);
                b->cond = w[3];
                b->ops[b->nops++] = a;
                b->ops[b->nops++] = c;
                collect_leaves(ids, w[3], b);
                break;
            }
            case OpLoad: {
                uint32_t id = w[2], ptr = w[3];
                if (ids[ptr].kind == ID_PTR && ids[ptr].dynIndex) {
                    /* An element picked at run time: loaded by a body op (per column
                     * for a matrix) once VI08 holds its address. */
                    int cols = matrix_cols(ids, w[1]), size = type_size(ids, w[1]);
                    if (cols) {
                        ids[id].kind = ID_VALUE;
                        ids[id].valType = w[1];
                        ids[id].dynCol0 = sh->nextId;
                        for (int c = 0; c < cols; c++)
                            define_dyn_load(ids, sh, new_id(sh), ids[w[1]].elemType, ptr, c);
                    } else if (size > 1 && ids[ptr].comp) {
                        /* A vec2/vec3 past field x: copied to the leading fields like a
                         * constant one. */
                        uint32_t q = new_id(sh);
                        Field fl[4];
                        define_dyn_load(ids, sh, q, w[1], ptr, 0);
                        for (int j = 0; j < size; j++) {
                            fl[j].src = q;
                            fl[j].comp = ids[ptr].comp + j;
                        }
                        define_map(ids, sh, OpLoad, id, w[1], fl, size);
                    } else if (size) {
                        define_dyn_load(ids, sh, id, w[1], ptr, 0);
                        ids[id].comp = ids[ptr].comp;
                    } else {
                        fail("vertex shader reads something other than a float, vector or matrix "
                             "at an index computed at run time");
                    }
                    break;
                }
                if (ids[ptr].kind == ID_PTR && ids[ptr].fromConst >= 0) {
                    /* A push constant member: its qword is loaded into a register in
                     * the prologue, so this reads that register in place... */
                    int size = type_size(ids, w[1]);
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    if (matrix_cols(ids, w[1])) {
                        /* A whole matrix: its columns are the qwords from fromConst on. */
                        ids[id].fromConst = ids[ptr].fromConst;
                        break;
                    }
                    if (size > 1 && ids[ptr].comp) {
                        /* ...unless it is a vec2/vec3 past field x (vec2 at offset 8):
                         * arithmetic expects vectors in the leading fields, so copy it. */
                        Field fl[4];
                        for (int j = 0; j < size; j++) {
                            fl[j].src = sh->constBase + (uint32_t)ids[ptr].fromConst;
                            fl[j].comp = ids[ptr].comp + j;
                        }
                        define_map(ids, sh, OpLoad, id, w[1], fl, size);
                        break;
                    }
                    ids[id].fromConst = ids[ptr].fromConst;
                    ids[id].comp = ids[ptr].comp;
                    break;
                }
                if (ptr && ptr == sh->samplerVar) {
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].texel = TEXEL_SAMPLER;
                    break;
                }
                if (ptr && ptr == sh->instanceIndexVar) {
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].instanceIndex = 1;
                    break;
                }
                if ((ids[ptr].kind != ID_VAR && ids[ptr].kind != ID_PTR) || ids[ptr].fromInput < 0)
                    fail("%s shader reads something other than a vertex input, push constant or uniform", stage);
                ids[id].kind = ID_VALUE;
                ids[id].valType = w[1];
                ids[id].fromInput = ids[ptr].fromInput;
                ids[id].comp = ids[ptr].comp;   /* in.w loaded through an access chain */
                break;
            }
            case OpImageSampleImplicitLod: {
                /* texture(tex, vUV): the GS does it, provided the coordinate is the
                 * interpolated vec2 input as is. */
                uint32_t id = w[2], sampler = w[3], coord = w[4];

                if (sh->execModel != ExecFragment)
                    unsupported(op, stage);
                if (ids[sampler].kind != ID_VALUE || ids[sampler].texel != TEXEL_SAMPLER)
                    fail("corrupt SPIR-V: texture() called on something other than the sampler");
                if (ids[coord].kind != ID_VALUE || ids[coord].fromInput != sh->texcoordInput ||
                    ids[coord].comp || n != 5)
                    fail("fragment shader samples with a coordinate other than its vec2 input "
                         "as is; the GS reads S and T straight from the vertex");
                ids[id].kind = ID_VALUE;
                ids[id].valType = w[1];
                ids[id].texel = TEXEL_SAMPLE;
                break;
            }
            case OpConvertSToF: case OpConvertUToF: {
                /* float(i): the int is already a float to VU1 (gl_InstanceIndex comes
                 * from the batch header as one). */
                uint32_t id = w[2], src = w[3];

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                if (ids[src].kind == ID_VALUE && ids[src].instanceIndex) {
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].fromConst = INSTANCE_QWORD;
                    ids[id].comp = 2;
                    break;
                }
                if (!is_float(ids, w[1]) || value_size(ids, src, stage) != 1)
                    fail("corrupt SPIR-V: float() of something other than an int");
                alias_value(ids, id, w[1], int_operand(ids, sh, src, sh->body, &sh->bodyCount));
                break;
            }
            case OpConvertFToS: case OpConvertFToU: {
                /* int(f): truncated where it is counted with, or used as an array or
                 * column index, where FTOI0 makes the address. */
                uint32_t id = w[2], src = w[3];

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                if (value_size(ids, src, stage) != 1)
                    fail("corrupt SPIR-V: int() of something other than a float");
                if (ids[src].kind == ID_CONST) {
                    float v;
                    memcpy(&v, &ids[src].comps[0], sizeof(v));
                    ids[id].kind = ID_CONST;
                    ids[id].valType = w[1];
                    ids[id].value = (uint32_t)(int32_t)v;
                    v = (float)(int32_t)v;
                    memcpy(&ids[id].comps[0], &v, sizeof(v));
                    ids[id].compCount = 1;
                    break;
                }
                ids[id].kind = ID_VALUE;
                ids[id].valType = w[1];
                ids[id].floatIndex = src;
                ids[id].comp = ids[src].comp;
                break;
            }
            case OpCompositeInsert: {
                /* v.w = x: the vector's fields with one replaced. */
                uint32_t id = w[2], obj = w[3], src = w[4];
                int size, k;
                Field fl[4];

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                size = value_size(ids, src, stage);
                if (n != 6 || size < 2 || w[5] >= (uint32_t)size || value_size(ids, obj, stage) != 1)
                    fail("vertex shader writes a component of something other than a float vector; "
                         "only v.x .. v.w = <float> is supported so far");
                k = (int)w[5];
                for (int j = 0; j < size; j++)
                    fl[j] = field_of(ids, sh, j == k ? obj : src, j == k ? 0 : j);
                define_map(ids, sh, op, id, w[1], fl, size);
                break;
            }
            case OpFAdd: case OpFSub: case OpFMul: case OpFDiv: case OpFNegate:
            case OpVectorTimesScalar:
            case OpIAdd: case OpISub: case OpIMul: case OpSNegate: {
                BodyOp *b;
                int size, isInt = op == OpIAdd || op == OpISub || op == OpIMul || op == OpSNegate;
                uint32_t a = w[3], c = w[4];

                if (isInt) {
                    /* Counting is float arithmetic to VU1. */
                    if (sh->execModel != ExecVertex)
                        unsupported(op, stage);
                    a = int_operand(ids, sh, a, sh->body, &sh->bodyCount);
                    if (op != OpSNegate)
                        c = int_operand(ids, sh, c, sh->body, &sh->bodyCount);
                    op = op == OpIAdd ? OpFAdd : op == OpISub ? OpFSub : op == OpIMul ? OpFMul : OpFNegate;
                    ids[w[2]].intIsFloat = 1;
                }
                if (sh->execModel == ExecFragment && op == OpFMul) {
                    /* texture(...) * vColor, either way round: GS modulate. */
                    uint32_t a = w[3], c = w[4];
                    if (ids[a].kind == ID_VALUE && ids[a].texel == TEXEL_SAMPLE &&
                        ids[c].kind == ID_VALUE && ids[c].fromInput == sh->colorInput && sh->colorInput >= 0) {
                        /* a * c as written */
                    } else if (ids[c].kind == ID_VALUE && ids[c].texel == TEXEL_SAMPLE &&
                               ids[a].kind == ID_VALUE && ids[a].fromInput == sh->colorInput && sh->colorInput >= 0) {
                        /* c * a */
                    } else {
                        fail("fragment shader multiplies something other than the texture sample "
                             "by the color input; the GS can only modulate the texture by the "
                             "vertex color");
                    }
                    ids[w[2]].kind = ID_VALUE;
                    ids[w[2]].valType = w[1];
                    ids[w[2]].texel = TEXEL_SAMPLE_TIMES_COLOR;
                    break;
                }
                if (sh->execModel == ExecFragment && (op == OpFAdd || op == OpFSub) &&
                    ((ids[w[3]].kind == ID_VALUE && ids[w[3]].texel == TEXEL_SAMPLE) ||
                     (ids[w[4]].kind == ID_VALUE && ids[w[4]].texel == TEXEL_SAMPLE)))
                    fail("fragment shader adds to the texture sample; the GS combines a texture "
                         "with the vertex color only by multiplying (modulate) or ignoring it (decal)");
                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                ids[w[2]].kind = ID_VALUE;
                ids[w[2]].valType = w[1];
                size = value_size(ids, w[2], stage);
                if (op == OpVectorTimesScalar) {
                    if (value_size(ids, a, stage) != size || value_size(ids, c, stage) != 1)
                        fail("corrupt SPIR-V: OpVectorTimesScalar needs a vector and a float");
                } else if (value_size(ids, a, stage) != size ||
                           (op != OpFNegate && value_size(ids, c, stage) != size)) {
                    fail("corrupt SPIR-V: %s operands differ in size", op_name(op));
                }
                /* A scalar result stays in the field its first operand uses; a division
                 * is copied out of Q into x. */
                if (size == 1)
                    ids[w[2]].comp = op == OpFDiv ? 0 : ids[a].comp;
                b = new_op(sh->body, &sh->bodyCount, op, w[2]);
                b->ops[0] = a;
                b->nops = 1;
                if (op != OpFNegate)
                    b->ops[b->nops++] = c;
                break;
            }
            case OpMatrixTimesVector: {
                /* m * v: one multiply-add per column, summed in the accumulator. The
                 * columns are plain vec operands (push constant qwords or vector
                 * constants), so the register allocator treats them like any other. */
                uint32_t id = w[2], m = w[3], v = w[4];
                int cols = matrix_cols(ids, ids[m].valType), rows, size;
                BodyOp *b;

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                if (!cols)
                    fail("corrupt SPIR-V: OpMatrixTimesVector operand is not a float matrix");
                rows = type_size(ids, ids[ids[m].valType].elemType);
                ids[id].kind = ID_VALUE;
                ids[id].valType = w[1];
                size = type_size(ids, w[1]);
                if (size != rows || value_size(ids, v, stage) != cols)
                    fail("corrupt SPIR-V: OpMatrixTimesVector sizes do not match");
                if (sh->bodyCount == MAX_BODY)
                    fail("vertex shader is too long (max %d instructions)", MAX_BODY);
                b = &sh->body[sh->bodyCount++];
                b->op = op;
                b->result = id;
                b->line = g_line;
                if (ids[m].kind == ID_CONST) {
                    if (ids[m].memberCount != cols)
                        fail("corrupt SPIR-V: constant matrix has the wrong number of columns");
                    for (int c = 0; c < cols; c++)
                        b->ops[c] = ids[m].members[c];
                } else if (ids[m].kind == ID_VALUE && ids[m].fromConst >= 0) {
                    for (int c = 0; c < cols; c++)
                        b->ops[c] = sh->constBase + (uint32_t)(ids[m].fromConst + c);
                } else if (ids[m].kind == ID_VALUE && ids[m].dynCol0) {
                    for (int c = 0; c < cols; c++)
                        b->ops[c] = ids[m].dynCol0 + (uint32_t)c;
                } else {
                    fail("vertex shader multiplies by a matrix that is neither a push constant, "
                         "a uniform nor a constant; only those are supported so far");
                }
                b->ops[cols] = v;
                b->nops = cols + 1;
                break;
            }
            case OpDot: case OpExtInst: {
                uint32_t id = w[2], ext = op == OpExtInst ? w[4] : 0;
                const uint32_t *args = op == OpExtInst ? &w[5] : &w[3];
                int nargs = op == OpExtInst ? (int)(n - 5) : 2, size, argSize;
                const ExtFn *fn = NULL;
                const char *name = "dot";
                BodyOp *b;

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                if (op == OpExtInst) {
                    if (w[3] != sh->glsl450)
                        fail("corrupt SPIR-V: OpExtInst from an instruction set other than GLSL.std.450");
                    fn = ext_fn(ext);
                    if (!fn)
                        fail("vertex shader calls GLSL.std.450 instruction %u, which this version "
                             "of ps2vk-shaderc does not compile", (unsigned)ext);
                    name = fn->name;
                    if (fn->status == EXT_NO_LOG)
                        fail("vertex shader calls %s(): VU1 has no logarithm unit", name);
                    if (fn->status == EXT_INTEGER)
                        fail("vertex shader calls %s() on integers; VU1 computes with floats only", name);
                    if (fn->status == EXT_TODO)
                        fail("vertex shader calls %s(), which this version of ps2vk-shaderc does "
                             "not compile yet", name);
                    if (nargs != fn->nargs)
                        fail("corrupt SPIR-V: %s() called with %d arguments", name, nargs);
                }
                ids[id].kind = ID_VALUE;
                ids[id].valType = w[1];
                size = value_size(ids, id, stage);
                argSize = value_size(ids, args[0], stage);
                for (int i = 1; i < nargs; i++)
                    if (value_size(ids, args[i], stage) != argSize)
                        fail("corrupt SPIR-V: %s() arguments differ in size", name);
                if (op == OpDot || ext == GlslLength) {
                    if (argSize < 2)
                        fail("vertex shader calls %s() on a float; it takes a vec2, vec3 or vec4", name);
                    if (size != 1)
                        fail("corrupt SPIR-V: %s() result is not a float", name);
                    ids[id].comp = 0;
                } else if (ext == GlslNormalize) {
                    if (argSize < 2)
                        fail("vertex shader calls normalize() on a float; it takes a vec2, vec3 or vec4");
                    if (size != argSize)
                        fail("corrupt SPIR-V: normalize() result differs in size from its argument");
                } else if (ext == GlslCross) {
                    if (argSize != 3 || size != 3)
                        fail("vertex shader calls cross() on something other than two vec3s");
                } else {
                    if (size != argSize)
                        fail("corrupt SPIR-V: %s() result differs in size from its arguments", name);
                    if (size == 1)
                        ids[id].comp = ext == GlslSqrt || ext == GlslInverseSqrt ? 0 : ids[args[0]].comp;
                }
                if (sh->bodyCount == MAX_BODY)
                    fail("vertex shader is too long (max %d instructions)", MAX_BODY);
                b = &sh->body[sh->bodyCount++];
                b->op = op;
                b->ext = ext;
                b->result = id;
                b->line = g_line;
                for (int i = 0; i < nargs; i++)
                    b->ops[i] = args[i];
                b->nops = nargs;
                break;
            }
            case OpCompositeExtract: {
                /* v.w */
                uint32_t id = w[2], src = w[3];
                Field fl[1];
                int size;

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                size = value_size(ids, src, stage);
                if (n != 5 || size < 2 || w[4] >= (uint32_t)size || !is_float(ids, w[1]))
                    fail("vertex shader reads a component of something other than a vector; "
                         "only v.x .. v.w of a float vector is supported so far");
                fl[0] = field_of(ids, sh, src, (int)w[4]);
                define_map(ids, sh, op, id, w[1], fl, 1);
                break;
            }
            case OpVectorShuffle: {
                /* v.bgra, vec4(a, b).xz: result fields picked from two vectors. */
                uint32_t id = w[2], v1 = w[3], v2 = w[4];
                int size1, size2, size = (int)(n - 5);
                Field fl[4];

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                size1 = value_size(ids, v1, stage);
                size2 = value_size(ids, v2, stage);
                if (size < 2 || size > 4 || type_size(ids, w[1]) != size)
                    fail("corrupt SPIR-V: OpVectorShuffle result is not a vec2, vec3 or vec4");
                for (int i = 0; i < size; i++) {
                    uint32_t s = w[5 + i];
                    if (s == SHUFFLE_UNDEF) {
                        fl[i].src = 0;
                        fl[i].comp = FIELD_UNDEF;
                    } else if (s < (uint32_t)size1) {
                        fl[i] = field_of(ids, sh, v1, (int)s);
                    } else if (s < (uint32_t)(size1 + size2)) {
                        fl[i] = field_of(ids, sh, v2, (int)(s - size1));
                    } else {
                        fail("corrupt SPIR-V: OpVectorShuffle component out of range");
                    }
                }
                define_map(ids, sh, op, id, w[1], fl, size);
                break;
            }
            case OpCompositeConstruct: {
                /* vec4(v.xyz, 1.0), vec4(r, g, b, a): operands laid end to end. */
                uint32_t id = w[2];
                int size, total = 0;
                Field fl[4];

                if (sh->execModel != ExecVertex)
                    unsupported(op, stage);
                size = type_size(ids, w[1]);
                if (ids[w[1]].type == T_MATRIX)
                    fail("vertex shader builds a matrix from values computed at run time; "
                         "only push constant, uniform and constant matrices are supported so far");
                if (!size || n - 3 > 4)
                    fail("vertex shader constructs something other than a vec2, vec3 or vec4 "
                         "from floats and vectors; only those are supported so far");
                for (uint32_t i = 0; i < n - 3; i++) {
                    int k = value_size(ids, w[3 + i], stage);
                    if (total + k > size)
                        fail("corrupt SPIR-V: OpCompositeConstruct operands overflow the result");
                    for (int j = 0; j < k; j++)
                        fl[total + j] = field_of(ids, sh, w[3 + i], j);
                    total += k;
                }
                if (total != size)
                    fail("corrupt SPIR-V: OpCompositeConstruct operands do not fill the result");
                define_map(ids, sh, op, id, w[1], fl, size);
                break;
            }
            case OpAccessChain: {
                uint32_t id = w[2], base = w[3], blockBytes = 0;
                int blockQword = 0;
                const char *kind = const_block(sh, base, &blockQword, &blockBytes);
                if (kind) {
                    /* pc.member, pc.array[i], pc.member.z: a constant byte offset into
                     * the block, which VU1 memory holds as qwords. */
                    uint32_t type = ids[ids[base].varType].elemType, off = 0, dynIndex = 0;
                    int size, dynStride = 0, dynCount = 0;

                    for (uint32_t i = 4; i < n; i++) {
                        uint32_t idx = w[i], v;
                        if (ids[idx].kind != ID_CONST) {
                            /* pc.array[i], pc.m[i]: VI08 = i * qwords per element, added
                             * to the element's static address, i clamped to the array like
                             * robustBufferAccess. The index is an int block member,
                             * gl_InstanceIndex or int() of a float. */
                            if (dynIndex)
                                fail("vertex shader reads %ss with two indices computed at run time; "
                                     "only one per read is supported", kind);
                            if (ids[idx].kind != ID_VALUE ||
                                !(ids[idx].floatIndex || ids[idx].instanceIndex ||
                                  is_int(ids, ids[idx].valType)))
                                fail("vertex shader indexes %ss with something other than an int; "
                                     "VU1 addresses qwords through VI08", kind);
                            if (ids[type].type == T_ARRAY) {
                                if (ids[type].stride % 16)
                                    fail("vertex shader indexes a %s array of %u-byte elements at run "
                                         "time; VU1 addresses qwords, so make them vec4 or use std140",
                                         kind, (unsigned)ids[type].stride);
                                dynStride = (int)(ids[type].stride / 16);
                            } else if (ids[type].type == T_MATRIX) {
                                dynStride = 1;
                            } else if (ids[type].type == T_VECTOR) {
                                fail("vertex shader picks a %s vector's component at run time; VU1 "
                                     "addresses qwords, so index an array of floats instead", kind);
                            } else {
                                fail("corrupt SPIR-V: %s access chain indexes a scalar", kind);
                            }
                            dynIndex = idx;
                            dynCount = (int)ids[type].elemCount;
                            type = ids[type].elemType;
                            continue;
                        }
                        v = ids[idx].value;
                        if (ids[type].type == T_STRUCT) {
                            if (v >= (uint32_t)ids[type].memberCount)
                                fail("corrupt SPIR-V: %s member index out of range", kind);
                            off += member_offset(type, v);
                            type = ids[type].members[v];
                        } else if (ids[type].type == T_ARRAY) {
                            if (v >= ids[type].elemCount)
                                fail("vertex shader reads element %u of a %s array "
                                     "of %u", (unsigned)v, kind, (unsigned)ids[type].elemCount);
                            off += ids[type].stride * v;
                            type = ids[type].elemType;
                        } else if (ids[type].type == T_MATRIX) {
                            /* pc.m[c]: column c is the c-th qword of the matrix. */
                            if (v >= ids[type].elemCount)
                                fail("vertex shader reads column %u of a %s matrix "
                                     "with %u columns", (unsigned)v, kind, (unsigned)ids[type].elemCount);
                            off += 16 * v;
                            type = ids[type].elemType;
                        } else if (ids[type].type == T_VECTOR) {
                            if (v >= ids[type].elemCount)
                                fail("corrupt SPIR-V: %s component index out of range", kind);
                            off += 4 * v;
                            type = ids[type].elemType;
                        } else {
                            fail("corrupt SPIR-V: %s access chain indexes a scalar", kind);
                        }
                    }
                    if (matrix_cols(ids, type)) {
                        /* pc.m as a whole, for matrix * vector: every column's qword is used. */
                        int cols = matrix_cols(ids, type);
                        if (off + 16 * (uint32_t)cols > blockBytes)
                            fail("corrupt SPIR-V: %s read past the end of the block", kind);
                        ids[id].kind = ID_PTR;
                        ids[id].fromConst = blockQword + (int)(off / 16);
                        ids[id].dynIndex = dynIndex;
                        ids[id].dynStride = dynStride;
                        ids[id].dynCount = dynCount;
                        break;
                    }
                    size = type_size(ids, type);
                    if (!size && ids[type].type == T_INT)
                        size = 1;   /* loadable; any arithmetic on it is rejected */
                    if (!size)
                        fail("vertex shader reads a whole %s block or array; "
                             "read its float, vec2, vec3 or vec4 members one at a time", kind);
                    if (off % 16 + 4 * (uint32_t)size > 16)
                        fail("%s member at byte %u crosses a 16-byte boundary, "
                             "which VU1 cannot read as one value", kind, (unsigned)off);
                    if (off + 4 * (uint32_t)size > blockBytes)
                        fail("corrupt SPIR-V: %s read past the end of the block", kind);
                    ids[id].kind = ID_PTR;
                    ids[id].fromConst = blockQword + (int)(off / 16);
                    ids[id].comp = (int)(off % 16 / 4);
                    ids[id].dynIndex = dynIndex;
                    ids[id].dynStride = dynStride;
                    ids[id].dynCount = dynCount;
                    break;
                }
                if (ids[base].kind == ID_VAR && ids[base].fromInput >= 0) {
                    /* in.w read straight from the input: glslang emits this instead of
                     * OpCompositeExtract when debug info is on. */
                    if (sh->execModel != ExecVertex)
                        unsupported(OpCompositeExtract, stage);
                    if (n != 5 || ids[w[4]].kind != ID_CONST || ids[w[4]].value >= 4)
                        fail("vertex shader indexes an input with something other than a "
                             "constant component; only in.x .. in.w is supported so far");
                    ids[id].kind = ID_PTR;
                    ids[id].fromInput = ids[base].fromInput;
                    ids[id].comp = (int)ids[w[4]].value;
                    break;
                }
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
                    if (ids[val].kind != ID_VALUE)
                        fail("fragment shader stores a computed value; only the color input, "
                             "the texture sample or their product can be written");
                    if (ids[val].texel == TEXEL_NONE && ids[val].fromInput != sh->colorInput)
                        fail("fragment shader writes its vec2 input as the color; only the color "
                             "input, the texture sample or their product can be written");
                    if (ids[val].texel == TEXEL_SAMPLER)
                        fail("corrupt SPIR-V: fragment shader stores the sampler itself");
                    if (ptr != sh->colorVar)
                        fail("fragment shader writes to something other than its output");
                    sh->colorValue = val;
                    sh->textureFunction = ids[val].texel == TEXEL_SAMPLE ? 1 : 0;
                    break;
                }
                {
                    /* Kept as an op: a second store to the same output (on another
                     * path) turns both into moves to one register (rebuild_phis). */
                    BodyOp *b;
                    int out;
                    if (ids[ptr].isPosition) {
                        if (value_size(ids, val, stage) != 4)
                            fail("vertex shader stores something other than a vec4 to gl_Position");
                        out = OUT_POSITION;
                    } else if (ptr && ptr == sh->colorVar) {
                        if (value_size(ids, val, stage) != 4)
                            fail("vertex shader stores something other than a vec4 to the color output");
                        out = OUT_COLOR;
                    } else if (ptr && ptr == sh->texcoordVar) {
                        if (value_size(ids, val, stage) != 2)
                            fail("vertex shader stores something other than a vec2 to the texture "
                                 "coordinate output");
                        out = OUT_TEXCOORD;
                    } else {
                        fail("%s shader writes to something other than its outputs", stage);
                    }
                    b = new_op(sh->body, &sh->bodyCount, OpStore, 0);
                    b->outVar = out;
                    b->ops[b->nops++] = val;
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
        if (sh->returns != 1)
            fail("vertex shader returns in %d places; VU1 code has one exit, so return once at "
                 "the end (use if/else instead of an early return)", sh->returns);
        rebuild_phis(ids, sh);
    }

    /* A map read only by other maps was folded into them: emit nothing for it. */
    {
        static int used[MAX_IDS];
        memset(used, 0, sizeof(used));
        for (int i = 0; i < sh->bodyCount; i++)
            if (!is_map_op(sh->body[i].op))
                for (int j = 0; j < sh->body[i].nops; j++)
                    used[root(ids, sh->body[i].ops[j])] = 1;
        if (sh->positionValue)
            used[root(ids, sh->positionValue)] = 1;
        if (sh->colorValue)
            used[root(ids, sh->colorValue)] = 1;
        if (sh->texcoordValue)
            used[root(ids, sh->texcoordValue)] = 1;
        for (int i = 0; i < sh->bodyCount; i++)
            if (is_map_op(sh->body[i].op) && !used[sh->body[i].result])
                sh->body[i].dead = 1;
    }

    if (sh->execModel == ExecVertex) {
        place_const_loads(ids, sh);
        /* A scalar 0 or 1 reads VF00 = (0, 0, 0, 1) instead of taking a register. */
        for (uint32_t i = 0; i < sh->constBase; i++) {
            if (ids[i].kind != ID_CONST || ids[i].compCount != 1)
                continue;
            if (!(ids[i].comps[0] & 0x7fffffff)) {
                ids[i].vf00 = 1;
                ids[i].comp = 0;
            } else if (ids[i].comps[0] == 0x3f800000) {
                ids[i].vf00 = 1;
                ids[i].comp = 3;
            }
        }
    }

    if (sh->execModel == ExecVertex) {
        if (!sh->positionValue)
            fail("vertex shader never writes gl_Position");
        if (sh->colorVar && !sh->colorValue)
            fail("vertex shader declares a color output but never writes it");
        if (sh->texcoordVar && !sh->texcoordValue)
            fail("vertex shader declares a texture coordinate output but never writes it");
    } else if (!sh->colorValue) {
        fail("fragment shader must write 'outColor = <color input>;' or "
             "'outColor = texture(<sampler>, <texcoord input>);'");
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

/* Culling: when a vertex completes a triangle with the two before it (VF25,
 * VF26), a negative signed area * winding sign sets ADC in its XYZ2. */
static void emit_cull(FILE *f, const char *sym)
{
    char buf[64];

    fprintf(f, "; culling: test the triangle this vertex completes\n");
    vu(f, "sub.w         VF23,VF00,VF00", "isubiu        VI11,VI11,1", "ADC off; one vertex closer to a triangle");
    snprintf(buf, sizeof(buf), "ibeq          VI12,VI00,%s_xyz", sym);
    vu(f, "NOP", buf, "no culling");
    snprintf(buf, sizeof(buf), "ilw.y         VI08,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_CULL);
    vu(f, "NOP", buf, "delay slot: 1 for a fan");
    snprintf(buf, sizeof(buf), "ibne          VI11,VI00,%s_shift", sym);
    vu(f, "NOP", buf, "triangle not complete yet");
    vu(f, "NOP", "NOP", "branch delay slot");
    vu(f, "sub.xy        VF20,VF26,VF25", "iadd          VI11,VI12,VI00", "P1 - P0; vertices to the next triangle");
    snprintf(buf, sizeof(buf), "lq            VF24,%d(VI01)", PVK_VU1_INPUT_COUNT);
    vu(f, "sub.xy        VF21,VF22,VF25", buf, "P2 - P0; batch header, z = winding sign");
    vu(f, "opmula.xyz    ACC,VF20,VF21", "NOP", NULL);
    vu(f, "opmsub.xyz    VF20,VF21,VF20", "NOP", "z = signed area");
    snprintf(buf, sizeof(buf), "lq            VF21,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_CULL);
    vu(f, "mul.z         VF20,VF20,VF24", buf, "area * sign, negative = cull; cull context");
    vu(f, "NOP", "iaddiu        VI13,VI00,0x20", "MAC flags land 4 cycles later; z negative flag");
    vu(f, "NOP", "NOP", NULL);
    vu(f, "NOP", "NOP", NULL);
    vu(f, "NOP", "NOP", NULL);
    vu(f, "mulx.z        VF24,VF24,VF21", "fmand         VI13,VI13", "next triangle's sign; sign flag of z");
    snprintf(buf, sizeof(buf), "sq            VF24,%d(VI01)", PVK_VU1_INPUT_COUNT);
    vu(f, "NOP", buf, NULL);
    snprintf(buf, sizeof(buf), "ibeq          VI13,VI00,%s_shift", sym);
    vu(f, "NOP", buf, "front facing: draw");
    snprintf(buf, sizeof(buf), "ilw.w         VI13,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_CULL);
    vu(f, "NOP", buf, "delay slot: ADC bit");
    vu(f, "NOP", "NOP", NULL);
    vu(f, "NOP", "mfir.w        VF23,VI13", "ADC on: the GS keeps the vertex but draws nothing");
    fprintf(f, "%s_shift:\n", sym);
    vu(f, "NOP", "isubiu        VI13,VI11,2", "0 at the batch's first vertex");
    snprintf(buf, sizeof(buf), "ibeq          VI08,VI00,%s_p0", sym);
    vu(f, "NOP", buf, "not a fan: P0 <- P1");
    vu(f, "NOP", "NOP", "branch delay slot");
    snprintf(buf, sizeof(buf), "ibne          VI13,VI00,%s_p1", sym);
    vu(f, "NOP", buf, "a fan's P0 stays its hub");
    vu(f, "NOP", "NOP", "branch delay slot");
    vu(f, "NOP", "move.xy       VF25,VF22", "the hub");
    snprintf(buf, sizeof(buf), "b             %s_p1", sym);
    vu(f, "NOP", buf, NULL);
    vu(f, "NOP", "NOP", "branch delay slot");
    fprintf(f, "%s_p0:\n", sym);
    vu(f, "NOP", "move.xy       VF25,VF26", NULL);
    fprintf(f, "%s_p1:\n", sym);
    vu(f, "NOP", "move.xy       VF26,VF22", "P1 <- this vertex");
    fprintf(f, "%s_xyz:\n", sym);
}

/*
 * VF registers free for constants and shader temporaries. The rest hold the
 * prologue constants (VF01-VF04), inputs (VF10-VF17), epilogue values (VF20-VF24)
 * and the previous two screen positions for culling (VF25-VF26).
 */
static const int g_pool[] = { 5, 6, 7, 8, 9, 18, 19, 27, 28, 29, 30, 31 };

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

/* The id whose register a value reads: its alias root, and for a push constant
 * or uniform value the synthetic id of its qword, which every load of it shares. */
static uint32_t canon(const IdInfo *ids, const Shader *sh, uint32_t id)
{
    id = root(ids, id);
    if (ids[id].kind == ID_VALUE && ids[id].fromConst >= 0)
        return sh->constBase + (uint32_t)ids[id].fromConst;
    return id;
}

/* Frees a temporary or a push constant/uniform qword after its last read
 * (inputs and constants stay). */
static void reg_release(Regs *r, const IdInfo *ids, const Shader *sh, uint32_t id, int at)
{
    id = canon(ids, sh, id);
    if (ids[id].kind == ID_VALUE && ids[id].fromInput < 0 && r->lastUse[id] <= at && r->reg[id]) {
        r->busy[r->reg[id]] = 0;
        r->reg[id] = 0;
    }
}

/* Register holding a value (an alias reads its root's register). */
static int reg_of(const Regs *r, const IdInfo *ids, const Shader *sh, uint32_t id)
{
    return r->reg[canon(ids, sh, id)];
}

/* Gives back every register whose value has been read for the last time: a
 * phi after its loop, a value kept alive across a back edge. */
static void reg_sweep(Regs *r, const IdInfo *ids, int at)
{
    for (uint32_t id = 0; id < MAX_IDS; id++)
        if (r->reg[id] && ids[id].kind == ID_VALUE && ids[id].fromInput < 0 && r->lastUse[id] < at) {
            r->busy[r->reg[id]] = 0;
            r->reg[id] = 0;
        }
}

/* Loads constant qword q (the push constants, then the uniform area) into reg. */
static void emit_const_load(FILE *f, const Shader *sh, int q, int reg)
{
    char buf[64], buf2[64];

    if (q == INSTANCE_QWORD) {
        snprintf(buf, sizeof(buf), "lq            VF%02d,%d(VI00)", reg, PVK_VU1_INPUT_ADDR + PVK_VU1_INPUT_STRIDE);
        snprintf(buf2, sizeof(buf2), "batch header, z = gl_InstanceIndex");
    } else if (q < PVK_VU1_PUSH_QWORDS) {
        snprintf(buf, sizeof(buf), "lq            VF%02d,%d(VI00)", reg, PVK_VU1_PUSH_ADDR + q);
        snprintf(buf2, sizeof(buf2), "push constants, bytes %d..%d", q * 16, q * 16 + 15);
    } else {
        int u = q - PVK_VU1_PUSH_QWORDS, b = 0;
        while (b + 1 < sh->uniformCount && sh->uniforms[b + 1].qword <= (uint32_t)u)
            b++;
        snprintf(buf, sizeof(buf), "lq            VF%02d,%d(VI00)", reg, PVK_VU1_UNIFORM_ADDR + u);
        snprintf(buf2, sizeof(buf2), "uniform '%s', bytes %d..%d", sh->uniforms[b].name,
                 (u - (int)sh->uniforms[b].qword) * 16, (u - (int)sh->uniforms[b].qword) * 16 + 15);
    }
    vu(f, "NOP", buf, buf2);
}

/* A push constant or uniform qword is loaded right before its first read and
 * its register given back after its last, so a shader may read more of them
 * than VU1 has registers to hold at once. */
static void reg_ensure_loaded(FILE *f, Regs *r, const IdInfo *ids, const Shader *sh, uint32_t id)
{
    uint32_t c = canon(ids, sh, id);

    if (c >= sh->constBase && !r->reg[c]) {
        r->reg[c] = reg_alloc(r);
        emit_const_load(f, sh, (int)(c - sh->constBase), r->reg[c]);
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

/*
 * Builds a field map into register d, one instruction per group of result
 * fields with the same source: a masked MOVE where the fields line up, else
 * 0 + source field broadcast; 0 and 1 come from VF00 = (0, 0, 0, 1).
 */
static void emit_map(FILE *f, const BodyOp *b, int d, const int *regs, int size, const char *line)
{
    static const char field[] = "xyzw";
    int done[4] = { 0, 0, 0, 0 }, first = 1;
    char buf[64], mask[5];

    for (int i = 0; i < size; i++) {
        int op = b->fieldOp[i], sc = b->fieldComp[i], k = 0, ident = op >= 0 && sc == i;

        if (done[i] || op == FIELD_UNDEF)
            continue;
        for (int j = i; j < size; j++) {
            int same = b->fieldOp[j] == op &&
                       (op < 0 || (ident ? b->fieldComp[j] == j : b->fieldComp[j] == sc));
            if (!done[j] && same) {
                mask[k++] = field[j];
                done[j] = 1;
            }
        }
        mask[k] = 0;
        if (op == FIELD_ZERO) {
            snprintf(buf, sizeof(buf), "sub.%-9s VF%02d,VF00,VF00", mask, d);
            vu(f, buf, "NOP", first ? line : NULL);
        } else if (op == FIELD_ONE) {
            snprintf(buf, sizeof(buf), "addw.%-8s VF%02d,VF00,VF00w", mask, d);
            vu(f, buf, "NOP", first ? line : NULL);
        } else if (ident) {
            snprintf(buf, sizeof(buf), "move.%-8s VF%02d,VF%02d", mask, d, regs[op]);
            vu(f, "NOP", buf, first ? line : NULL);
        } else {
            char mn[16];
            snprintf(mn, sizeof(mn), "add%c.%s", field[sc], mask);
            snprintf(buf, sizeof(buf), "%-14sVF%02d,VF00,VF%02d%c", mn, d, regs[op], field[sc]);
            vu(f, buf, "NOP", first ? line : NULL);
        }
        first = 0;
    }
}

/* Returns the pending comment once; later instructions of the same op get none. */
static const char *take(const char **comment)
{
    const char *s = *comment;
    *comment = NULL;
    return s;
}

/* "maxy.x", "abs.xyz": mnemonic, optional broadcast field, destination mask. */
static const char *mn(const char *base, char bc, const char *mask)
{
    static char s[24];
    if (bc)
        snprintf(s, sizeof(s), "%s%c.%s", base, bc, mask);
    else
        snprintf(s, sizeof(s), "%s.%s", base, mask);
    return s;
}

static void emit_u(FILE *f, const char **comment, const char *mnemonic, const char *fmt, ...)
{
    char ops[48], buf[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(ops, sizeof(ops), fmt, ap);
    va_end(ap);
    snprintf(buf, sizeof(buf), "%-14s%s", mnemonic, ops);
    vu(f, buf, "NOP", take(comment));
}

static void emit_l(FILE *f, const char **comment, const char *mnemonic, const char *fmt, ...)
{
    char ops[48], buf[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(ops, sizeof(ops), fmt, ap);
    va_end(ap);
    if (ops[0])
        snprintf(buf, sizeof(buf), "%-14s%s", mnemonic, ops);
    else
        snprintf(buf, sizeof(buf), "%s", mnemonic);
    vu(f, "NOP", buf, take(comment));
}

static void emit_loi(FILE *f, const char **comment, uint32_t bits)
{
    char ins[64], val[32];

    loi_text(bits, ins, sizeof(ins), val, sizeof(val));
    vu(f, "NOP", ins, *comment ? take(comment) : val);
}

/* d = a op b; a scalar broadcasts b's field. */
static void emit_bin(FILE *f, const char **comment, const char *base, const char *dest, int size,
                     int d, int a, int bb, int cb)
{
    static const char field[] = "xyzw";
    if (size == 1)
        emit_u(f, comment, mn(base, field[cb], dest), "VF%02d,VF%02d,VF%02d%c", d, a, bb, field[cb]);
    else
        emit_u(f, comment, mn(base, 0, dest), "VF%02d,VF%02d,VF%02d", d, a, bb);
}

/* d.x = dot(a, b) over n fields. */
static void emit_dot(FILE *f, const char **comment, int d, int a, int bb, int n)
{
    static const char field[] = "xyzw";
    emit_u(f, comment, mn("mul", 0, mask_n(n)), "VF%02d,VF%02d,VF%02d", d, a, bb);
    for (int i = 1; i < n; i++)
        emit_u(f, comment, mn("add", field[i], "x"), "VF%02d,VF%02d,VF%02d%c", d, d, d, field[i]);
}

/* d.c = Q. */
static void emit_from_q(FILE *f, const char **comment, int d, char c)
{
    char m[8];
    snprintf(m, sizeof(m), "%c", c);
    if (c == 'w')
        emit_u(f, comment, mn("mulq", 0, m), "VF%02d,VF00,Q", d);
    else
        emit_u(f, comment, mn("addq", 0, m), "VF%02d,VF00,Q", d);
}

/* floor(a) = (a + 1.5 * 2^23) - 1.5 * 2^23, relying on VU1 truncation; |a| < 2^22.
 * ceil subtracts first. */
static void emit_floor(FILE *f, const char **comment, const char *dest, int d, int a, int ceil)
{
    emit_loi(f, comment, 0x4b400000);
    emit_u(f, comment, mn(ceil ? "subi" : "addi", 0, dest), "VF%02d,VF%02d,I", d, a);
    emit_u(f, comment, mn(ceil ? "addi" : "subi", 0, dest), "VF%02d,VF%02d,I", d, d);
}

/* Runs an EFU instruction on each field of d in place, through P. */
static void emit_efu(FILE *f, const char **comment, const char *efu, const char *dest, int d)
{
    for (const char *c = dest; *c; c++) {
        char m[2] = { *c, 0 };
        emit_l(f, comment, efu, "P,VF%02d%c", d, *c);
        emit_l(f, comment, "waitp", "");
        emit_l(f, comment, mn("mfp", 0, m), "VF%02d,P", d);
    }
}

static const char *body_name(const BodyOp *b)
{
    static char s[32];
    if (b->op == OpExtInst) {
        snprintf(s, sizeof(s), "%s()", ext_fn(b->ext)->name);
        return s;
    }
    switch (b->op) {
    case OpDot: return "dot()";
    case OpSelect: return "?:";
    case OpBoolToFloat: return "condition kept as a float";
    case OpBranchConditional: return "if";
    case OpPhiMove: return "value of this path";
    case OpConvertSToF: return "int to float";
    default: return op_name(b->op);
    }
}

static int g_labels = 0;   /* short-circuit and ?: labels so far */

/* d.dc = s.sc (size 1) or d = s over the first size fields. */
static void emit_move(FILE *f, const char **comment, int d, int dc, int s, int sc, int size)
{
    static const char field[] = "xyzw";
    char m[2] = { field[dc], 0 };

    if (size > 1)
        emit_l(f, comment, mn("move", 0, mask_n(size)), "VF%02d,VF%02d", d, s);
    else if (dc == sc)
        emit_l(f, comment, mn("move", 0, m), "VF%02d,VF%02d", d, s);
    else
        emit_u(f, comment, mn("add", field[sc], m), "VF%02d,VF00,VF%02d%c", d, s, field[sc]);
}

/*
 * Leaves VI13 nonzero when the bool holds. A comparison subtracts into scratch
 * register t and reads the MAC flag of its field (sign for <, zero for ==) with
 * FMAND once the flags have settled; an inverted test subtracts the mask, so
 * VI13 is nonzero when the flag is clear. A bool held as a float is nonzero when
 * true. && and || short-circuit with branches. The next instruction must not
 * test VI13 (the integer pipeline shows it the old value).
 */
static void emit_cond(FILE *f, const char **comment, const IdInfo *ids, const int *regs, int *cur,
                      uint32_t cond, int t, const char *sym)
{
    const IdInfo *c = &ids[cond];
    int mask, inv, label;

    switch (c->boolKind) {
    case BOOL_CMP: case BOOL_FLOAT: {
        int a = regs[(*cur)++], ca = ids[c->boolA].comp;
        const char *dest = &"x\0y\0z\0w"[ca * 2];
        if (c->boolKind == BOOL_CMP) {
            int bb = regs[(*cur)++], cb = ids[c->boolB].comp;
            emit_bin(f, comment, "sub", dest, 1, t, a, bb, cb);
            mask = (c->boolZero ? 0x8 : 0x80) >> ca;
            inv = c->boolInv;
        } else {
            emit_u(f, comment, mn("add", 'x', dest), "VF%02d,VF%02d,VF00x", t, a);
            mask = 0x8 >> ca;
            inv = !c->boolInv;
        }
        emit_l(f, comment, "iaddiu", "VI13,VI00,0x%x", mask);
        vu(f, "NOP", "NOP", "MAC flags land 4 cycles later");
        vu(f, "NOP", "NOP", NULL);
        vu(f, "NOP", "NOP", NULL);
        emit_l(f, comment, "fmand", "VI13,VI13");
        if (inv)
            emit_l(f, comment, "isubiu", "VI13,VI13,0x%x", mask);
        break;
    }
    case BOOL_AND: case BOOL_OR:
        label = g_labels++;
        emit_cond(f, comment, ids, regs, cur, c->boolA, t, sym);
        vu(f, "NOP", "NOP", "VI13 settles");
        emit_l(f, comment, c->boolKind == BOOL_AND ? "ibeq" : "ibne", "VI13,VI00,%s_C%d", sym, label);
        vu(f, "NOP", "NOP", c->boolKind == BOOL_AND ? "&&: branch delay slot" : "||: branch delay slot");
        emit_cond(f, comment, ids, regs, cur, c->boolB, t, sym);
        fprintf(f, "%s_C%d:\n", sym, label);
        break;
    default:
        fail("corrupt SPIR-V: a branch on something other than a bool");
    }
}

/* dot() and the GLSL built-ins. d never aliases an operand, so it doubles as scratch;
 * a scalar result sits in the first operand's field (ca) and broadcasts cb, cc. */
static void emit_builtin(FILE *f, const BodyOp *b, int size, int argSize, const char *dest, int d,
                         const int *regs, int ca, int cb, int cc, const char *line)
{
    static const char field[] = "xyzw";
    const char *c = line;
    int a = regs[0], bb = regs[1], t = regs[2];
    uint32_t ext = b->op == OpDot ? 0 : b->ext;

    if (b->op == OpDot) {
        emit_dot(f, &c, d, a, bb, argSize);
        return;
    }
    switch (ext) {
    case GlslFAbs:
        emit_u(f, &c, mn("abs", 0, dest), "VF%02d,VF%02d", d, a);
        break;
    case GlslFMin: case GlslFMax:
        emit_bin(f, &c, ext == GlslFMin ? "mini" : "max", dest, size, d, a, bb, cb);
        break;
    case GlslFClamp:
        emit_bin(f, &c, "max", dest, size, d, a, bb, cb);
        emit_bin(f, &c, "mini", dest, size, d, d, t, cc);
        break;
    case GlslFMix:
        /* a + (b - a) * t */
        emit_u(f, &c, mn("mulaw", 0, dest), "ACC,VF%02d,VF00w", a);
        if (size == 1 && cb != ca) {
            emit_u(f, &c, mn("add", field[cb], dest), "VF%02d,VF00,VF%02d%c", d, bb, field[cb]);
            emit_u(f, &c, mn("sub", 0, dest), "VF%02d,VF%02d,VF%02d", d, d, a);
        } else {
            emit_u(f, &c, mn("sub", 0, dest), "VF%02d,VF%02d,VF%02d", d, bb, a);
        }
        emit_bin(f, &c, "madd", dest, size, d, d, t, cc);
        break;
    case GlslFloor: case GlslCeil:
        emit_floor(f, &c, dest, d, a, ext == GlslCeil);
        break;
    case GlslFract:
        emit_floor(f, &c, dest, d, a, 0);
        emit_u(f, &c, mn("sub", 0, dest), "VF%02d,VF%02d,VF%02d", d, a, d);
        break;
    case GlslTrunc:
        emit_u(f, &c, mn("ftoi0", 0, dest), "VF%02d,VF%02d", d, a);
        emit_u(f, &c, mn("itof0", 0, dest), "VF%02d,VF%02d", d, d);
        break;
    case GlslSqrt: case GlslInverseSqrt:
        for (int i = 0; i < size; i++) {
            char sa = size == 1 ? field[ca] : field[i], dc = size == 1 ? 'x' : field[i];
            if (ext == GlslSqrt)
                emit_l(f, &c, "sqrt", "Q,VF%02d%c", a, sa);
            else
                emit_l(f, &c, "rsqrt", "Q,VF00w,VF%02d%c", a, sa);
            emit_l(f, &c, "waitq", "");
            emit_from_q(f, &c, d, dc);
        }
        break;
    case GlslLength:
        emit_dot(f, &c, d, a, a, argSize);
        emit_l(f, &c, "sqrt", "Q,VF%02dx", d);
        emit_l(f, &c, "waitq", "");
        emit_from_q(f, &c, d, 'x');
        break;
    case GlslNormalize:
        emit_dot(f, &c, d, a, a, size);
        emit_l(f, &c, "rsqrt", "Q,VF00w,VF%02dx", d);
        emit_l(f, &c, "waitq", "");
        emit_u(f, &c, mn("mulq", 0, dest), "VF%02d,VF%02d,Q", d, a);
        break;
    case GlslCross:
        emit_u(f, &c, "opmula.xyz", "ACC,VF%02d,VF%02d", a, bb);
        emit_u(f, &c, "opmsub.xyz", "VF%02d,VF%02d,VF%02d", d, bb, a);
        break;
    case GlslSin: case GlslCos:
        /* ESIN holds for -pi..pi: x - 2pi * floor((x + pi) / 2pi); cos(x) = sin(x + pi/2). */
        if (ext == GlslCos) {
            emit_loi(f, &c, 0x3fc90fdb);
            emit_u(f, &c, mn("addi", 0, dest), "VF%02d,VF%02d,I", d, a);
            a = d;
        }
        emit_u(f, &c, mn("mulaw", 0, dest), "ACC,VF%02d,VF00w", a);
        emit_loi(f, &c, 0x40490fdb);
        emit_u(f, &c, mn("addi", 0, dest), "VF%02d,VF%02d,I", d, a);
        emit_loi(f, &c, 0x3e22f983);
        emit_u(f, &c, mn("muli", 0, dest), "VF%02d,VF%02d,I", d, d);
        emit_floor(f, &c, dest, d, d, 0);
        emit_loi(f, &c, 0x40c90fdb);
        emit_u(f, &c, mn("msubi", 0, dest), "VF%02d,VF%02d,I", d, d);
        emit_efu(f, &c, "esin", dest, d);
        break;
    case GlslExp: case GlslExp2:
        /* EEXP computes e^-x. */
        emit_loi(f, &c, ext == GlslExp ? 0xbf800000 : 0xbf317218);
        emit_u(f, &c, mn("muli", 0, dest), "VF%02d,VF%02d,I", d, a);
        emit_efu(f, &c, "eexp", dest, d);
        break;
    }
}

/* Emits body op b into register d; regs[] holds each operand's register. */
static void emit_body_op(FILE *f, const IdInfo *ids, const Shader *sh, const BodyOp *b, int d,
                         const int *regs)
{
    static const char field[] = "xyzw";
    int size = type_size(ids, ids[b->result].valType);
    int a = regs[0], bb = regs[1];
    int cols = b->nops - 1, vreg = cols > 0 ? regs[cols] : 0;   /* matrix * vector */
    int ca = ids[b->ops[0]].comp, cb = b->nops > 1 ? ids[b->ops[1]].comp : 0;
    int cc = b->nops > 2 ? ids[b->ops[2]].comp : 0;
    const char *dest = size == 1 ? &"x\0y\0z\0w"[ca * 2] : mask_n(size);
    char buf[64], line[96];
    int i;

    if (b->line)
        snprintf(line, sizeof(line), "line %u: %s", (unsigned)b->line, body_name(b));
    else
        snprintf(line, sizeof(line), "%s", body_name(b));

    switch (b->op) {
    case OpFAdd: case OpFSub: case OpFMul: {
        const char *m = b->op == OpFAdd ? "add" : b->op == OpFSub ? "sub" : "mul";
        char mn[16];
        if (size == 1) {
            /* d.ca = a.ca op b.cb: the second operand is broadcast from its field. */
            snprintf(mn, sizeof(mn), "%s%c.%s", m, field[cb], dest);
            snprintf(buf, sizeof(buf), "%-14sVF%02d,VF%02d,VF%02d%c", mn, d, a, bb, field[cb]);
        } else {
            snprintf(mn, sizeof(mn), "%s.%s", m, dest);
            snprintf(buf, sizeof(buf), "%-14sVF%02d,VF%02d,VF%02d", mn, d, a, bb);
        }
        vu(f, buf, "NOP", line);
        break;
    }
    case OpVectorTimesScalar: {
        char mn[16];
        snprintf(mn, sizeof(mn), "mul%c.%s", field[cb], dest);
        snprintf(buf, sizeof(buf), "%-14sVF%02d,VF%02d,VF%02d%c", mn, d, a, bb, field[cb]);
        vu(f, buf, "NOP", line);
        break;
    }
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
        for (i = 0; i < size; i++) {
            char c = field[i], sa = size == 1 ? field[ca] : c, sb = size == 1 ? field[cb] : c;
            snprintf(buf, sizeof(buf), "div           Q,VF%02d%c,VF%02d%c", a, sa, bb, sb);
            vu(f, "NOP", buf, i ? NULL : line);
            vu(f, "NOP", "waitq", NULL);
            if (c == 'w')
                snprintf(buf, sizeof(buf), "mulq.w        VF%02d,VF00,Q", d);
            else
                snprintf(buf, sizeof(buf), "addq.%c        VF%02d,VF00,Q", c, d);
            vu(f, buf, "NOP", NULL);
        }
        break;
    case OpMatrixTimesVector:
        /* d = c0 * v.x + c1 * v.y + ...: MULA starts the accumulator, MADDA adds to
         * it, MADD adds the last column and writes the result. */
        for (i = 0; i < cols; i++) {
            const char *m = i == 0 ? "mula" : i == cols - 1 ? "madd" : "madda";
            char mn[16], dst[8];
            snprintf(mn, sizeof(mn), "%s%c.%s", m, field[i], dest);
            if (i == cols - 1)
                snprintf(dst, sizeof(dst), "VF%02d", d);
            else
                snprintf(dst, sizeof(dst), "ACC");
            snprintf(buf, sizeof(buf), "%-14s%s,VF%02d,VF%02d%c", mn, dst, regs[i], vreg, field[i]);
            vu(f, buf, "NOP", i ? NULL : line);
        }
        break;
    case OpVectorShuffle: case OpCompositeConstruct: case OpCompositeExtract: case OpCompositeInsert:
        emit_map(f, b, d, regs, size, line);
        break;
    case OpConstLoad:
        emit_const_load(f, sh, (int)b->target, d);
        break;
    case OpConvertSToF: {
        const char *c = line;
        emit_u(f, &c, mn("itof0", 0, dest), "VF%02d,VF%02d", d, a);
        break;
    }
    case OpBoolToFloat: {
        /* The flag bits of the test, as a float: nonzero when true. */
        const char *c = line;
        int cur = 0;
        emit_cond(f, &c, ids, regs, &cur, b->cond, d, g_sym);
        vu(f, "NOP", "NOP", "VI13 settles");
        emit_l(f, &c, "mfir.x", "VF%02d,VI13", d);
        emit_u(f, &c, "itof0.x", "VF%02d,VF%02d", d, d);
        break;
    }
    case OpSelect: {
        /* d = b, then d = a if the condition holds. */
        const char *c = line;
        int cur = 2, label = g_labels++;
        emit_cond(f, &c, ids, regs, &cur, b->cond, d, g_sym);
        emit_move(f, &c, d, ca, bb, cb, size);   /* also lets VI13 settle */
        emit_l(f, &c, "ibeq", "VI13,VI00,%s_S%d", g_sym, label);
        vu(f, "NOP", "NOP", "branch delay slot");
        emit_move(f, &c, d, ca, a, ca, size);
        fprintf(f, "%s_S%d:\n", g_sym, label);
        break;
    }
    case OpDynLoad: {
        /* VI08 = clamp(index, 0, count - 1) * stride, then the element's qword(s)
         * relative to it. */
        const char *c = b->dynStride ? line : NULL;
        const char *fm = &"x\0y\0z\0w"[b->dynComp * 2];
        char fc = field[b->dynComp];
        if (b->dynStride) {
            if (b->dynInt) {
                emit_u(f, &c, mn("itof0", 0, fm), "VF%02d,VF%02d", d, a);
                a = d;
            }
            fprintf(f, "%s_Clamp%d:\n", g_sym, g_clamps++);
            emit_u(f, &c, mn("maxx", 0, fm), "VF%02d,VF%02d,VF00x", d, a);
            emit_loi(f, &c, b->dynLast);
            emit_u(f, &c, mn("minii", 0, fm), "VF%02d,VF%02d,I", d, d);
            emit_u(f, &c, mn("ftoi0", 0, fm), "VF%02d,VF%02d", d, d);
            emit_l(f, &c, "mtir", "VI08,VF%02d%c", d, fc);
            if (b->dynStride == 2 || b->dynStride == 4)
                emit_l(f, &c, "iadd", "VI08,VI08,VI08");
            if (b->dynStride == 4)
                emit_l(f, &c, "iadd", "VI08,VI08,VI08");
            if (b->dynStride == 3) {
                emit_l(f, &c, "iadd", "VI13,VI08,VI08");
                emit_l(f, &c, "iadd", "VI08,VI08,VI13");
            }
        }
        emit_l(f, &c, "lq", "VF%02d,%d(VI08)", d, b->dynBase);
        break;
    }
    case OpDot: case OpExtInst:
        emit_builtin(f, b, size, type_size(ids, ids[b->ops[0]].valType), dest, d, regs,
                     ca, cb, cc, line);
        break;
    }
}

static void emit_vertex(const Shader *sh, const IdInfo *ids, const char *base, const char *sym)
{
    FILE *f = open_out(base, ".vsm");
    char buf[64], buf2[64];
    static Regs r;
    static int def[MAX_IDS], labelAt[MAX_IDS], targeted[MAX_IDS];
    int i, constants = 0;

    g_sym = sym;

    /* Registers: inputs are fixed, constants live for the whole program,
     * temporaries and push constant/uniform qwords are freed after their last read. */
    memset(&r, 0, sizeof(r));
    memset(def, -1, sizeof(def));
    memset(labelAt, -1, sizeof(labelAt));
    memset(targeted, 0, sizeof(targeted));
    for (i = 0; i < sh->bodyCount; i++) {
        const BodyOp *b = &sh->body[i];
        if (b->dead)
            continue;
        for (int j = 0; j < b->nops; j++)
            r.lastUse[canon(ids, sh, b->ops[j])] = i;
        if (b->op == OpLabel)
            labelAt[b->result] = i;
        else if (b->result && def[b->result] < 0)
            def[b->result] = i;
    }
    r.lastUse[canon(ids, sh, sh->positionValue)] = MAX_BODY;
    if (sh->colorValue)
        r.lastUse[canon(ids, sh, sh->colorValue)] = MAX_BODY;
    if (sh->texcoordValue) {
        r.lastUse[canon(ids, sh, sh->texcoordValue)] = MAX_BODY;
        r.busy[PVK_VU1_TEXCOORD_REG] = 1;   /* the STQ output lives there */
    }
    /* A value from before a loop that the loop reads must outlive every
     * iteration: its last read becomes the branch back to the loop's start. */
    for (i = 0; i < sh->bodyCount; i++) {
        const BodyOp *b = &sh->body[i];
        if (!is_branch_op(b->op))
            continue;
        for (int t = 0; t < (b->op == OpBranchConditional ? 2 : 1); t++) {
            uint32_t label = t ? b->target2 : b->target;
            int h = labelAt[label];
            if (h < 0)
                fail("corrupt SPIR-V: branch to an unknown label");
            if (h < i) {
                for (int k = h; k <= i; k++)
                    for (int j = 0; j < sh->body[k].nops && !sh->body[k].dead; j++) {
                        uint32_t c = canon(ids, sh, sh->body[k].ops[j]);
                        if (def[c] >= 0 && def[c] < h && r.lastUse[c] < i)
                            r.lastUse[c] = i;
                    }
            }
            if (!(i + 1 < sh->bodyCount && sh->body[i + 1].op == OpLabel && sh->body[i + 1].result == label))
                targeted[label] = 1;
        }
    }
    for (i = 0; i < MAX_IDS; i++)
        if ((ids[i].kind == ID_VALUE || ids[i].kind == ID_VAR) && ids[i].fromInput >= 0)
            r.reg[i] = 10 + ids[i].fromInput;

    fprintf(f, "; Generated by ps2vk-shaderc from %s. Do not edit.\n", g_src);
    fprintf(f, ";\n; VU1 registers:\n");
    fprintf(f, ";   VF01 viewport scale, VF02 viewport offset, VF03 color scale\n");
    fprintf(f, ";   VF10.. shader inputs, VF20 gl_Position, VF21 color output\n");
    if (sh->texcoordValue)
        fprintf(f, ";   VF%02d texture coordinate output (S, T, Q, or U, V)\n", PVK_VU1_TEXCOORD_REG);
    fprintf(f, ";   VF25-VF26 the previous two screen positions (culling)\n");
    fprintf(f, ";   VF05-VF09, VF18-VF19, VF27-VF31 push constants, uniforms, constants and temporaries\n");
    fprintf(f, ";   VI02 vertices left, VI03 input pointer, VI04 output base, VI05 output pointer\n");
    fprintf(f, ";   VI06 index pointer, VI07 qwords per vertex, VI08 scratch, VI09 index list offset\n");
    fprintf(f, ";   (0 = in order), VI10 first vertex, VI11 vertices to the next triangle, VI12 vertices\n");
    fprintf(f, ";   per triangle (0 = no culling), VI13 scratch, VI14 instances left, VI15 instance pointer\n\n");
    fprintf(f, "\t\t.vu\n\t\t.align 4\n");
    fprintf(f, "\t\t.global\t%s_CodeStart\n\t\t.global\t%s_Convert\n\t\t.global\t%s_CodeEnd\n", sym, sym, sym);
    fprintf(f, "%s_CodeStart:\n", sym);

    fprintf(f, "; prologue: constants and the batch header\n");
    snprintf(buf, sizeof(buf), "lq            VF01,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_VIEWPORT_SCALE);
    vu(f, "NOP", buf, NULL);
    snprintf(buf, sizeof(buf), "lq            VF02,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_VIEWPORT_OFFSET);
    vu(f, "NOP", buf, NULL);
    snprintf(buf, sizeof(buf), "lq            VF03,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_COLOR_SCALE);
    vu(f, "NOP", buf, NULL);
    snprintf(buf, sizeof(buf), "iaddiu        VI01,VI00,%d", PVK_VU1_INPUT_ADDR);
    vu(f, "NOP", buf, "input base");
    snprintf(buf, sizeof(buf), "lq            VF04,%d(VI01)", PVK_VU1_INPUT_GIFTAG);
    vu(f, "NOP", buf, "GIF tag for each instance");
    snprintf(buf, sizeof(buf), "ilw.x         VI13,%d(VI01)", PVK_VU1_INPUT_FIRST);
    vu(f, "NOP", buf, "qword offset of the first vertex");
    snprintf(buf, sizeof(buf), "ilw.y         VI09,%d(VI01)", PVK_VU1_INPUT_COUNT);
    vu(f, "NOP", buf, "index list offset, 0 = vertices in order");
    vu(f, "NOP", "iadd          VI10,VI01,VI13", "first vertex");
    snprintf(buf, sizeof(buf), "ilw.x         VI07,%d(VI01)", PVK_VU1_INPUT_STRIDE);
    vu(f, "NOP", buf, "qwords per vertex");
    snprintf(buf, sizeof(buf), "ilw.z         VI14,%d(VI01)", PVK_VU1_INPUT_FIRST);
    vu(f, "NOP", buf, "instances");
    snprintf(buf, sizeof(buf), "ilw.w         VI15,%d(VI01)", PVK_VU1_INPUT_FIRST);
    vu(f, "NOP", buf, "qword offset of the first instance");
    snprintf(buf, sizeof(buf), "iaddiu        VI04,VI00,%d", PVK_VU1_OUTPUT_ADDR);
    vu(f, "NOP", buf, "output base");
    vu(f, "sub.xyzw      VF23,VF00,VF00", "iadd          VI15,VI01,VI15", "XYZ2 w = 0 (ADC off); instance pointer");
    vu(f, "NOP", "iadd          VI05,VI04,VI00", "output pointer");
    snprintf(buf, sizeof(buf), "ilw.z         VI12,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_CULL);
    vu(f, "NOP", buf, "vertices per triangle, 0 = no culling");

    for (i = 0; i <= sh->bodyCount + 2; i++) {
        uint32_t use[MAX_OPERANDS] = { 0 };
        if (i < sh->bodyCount) {
            for (int j = 0; j < sh->body[i].nops && !sh->body[i].dead; j++)
                use[j] = sh->body[i].ops[j];
        } else if (i == sh->bodyCount) {
            use[0] = sh->positionValue;
        } else if (i == sh->bodyCount + 1) {
            use[0] = sh->colorValue;
        } else {
            use[0] = sh->texcoordValue;
        }
        for (int j = 0; j < MAX_OPERANDS; j++) {
            uint32_t c = use[j] ? root(ids, use[j]) : 0, k;
            if (!c || ids[c].kind != ID_CONST || r.reg[c] || ids[c].vf00)
                continue;
            /* a constant with the same value already has a register */
            for (k = 0; k < sh->constBase; k++)
                if (k != c && ids[k].kind == ID_CONST && r.reg[k] && ids[k].compCount == ids[c].compCount &&
                    !memcmp(ids[k].comps, ids[c].comps, sizeof(uint32_t) * (size_t)ids[c].compCount))
                    break;
            if (k < sh->constBase) {
                r.reg[c] = r.reg[k];
                continue;
            }
            if (!constants++)
                fprintf(f, "; constants\n");
            r.reg[c] = reg_alloc(&r);
            emit_constant(f, &ids[c], r.reg[c]);
        }
    }

    fprintf(f, "%s_instance:\n", sym);
    fprintf(f, "; each instance restarts the primitive with a GIF tag of its own\n");
    vu(f, "NOP", "isubiu        VI14,VI14,1", "instances after this one");
    snprintf(buf, sizeof(buf), "ilw.x         VI08,%d(VI01)", PVK_VU1_INPUT_GIFTAG);
    vu(f, "NOP", buf, "NLOOP with EOP");
    vu(f, "NOP", "sq            VF04,0(VI05)", "the instance's GIF tag");
    vu(f, "NOP", "isubiu        VI08,VI08,0x4000", NULL);
    snprintf(buf, sizeof(buf), "ibeq          VI14,VI00,%s_eop", sym);
    vu(f, "NOP", buf, "the last instance keeps EOP");
    vu(f, "NOP", "isubiu        VI08,VI08,0x4000", "delay slot: EOP off");
    vu(f, "NOP", "isw.x         VI08,0(VI05)", "more instances follow");
    fprintf(f, "%s_eop:\n", sym);
    vu(f, "NOP", "iaddiu        VI05,VI05,1", NULL);
    snprintf(buf, sizeof(buf), "lq            VF24,%d(VI01)", PVK_VU1_INPUT_FIRST);
    vu(f, "NOP", buf, "y = winding sign of the first triangle");
    snprintf(buf, sizeof(buf), "ilw.x         VI02,%d(VI01)", PVK_VU1_INPUT_COUNT);
    vu(f, "NOP", buf, "vertices per instance");
    vu(f, "NOP", "iadd          VI03,VI10,VI00", "first vertex");
    vu(f, "NOP", "iadd          VI06,VI01,VI09", "index pointer");
    vu(f, "addy.z        VF24,VF00,VF24", "iaddiu        VI11,VI00,3", "the third vertex completes the first triangle");
    snprintf(buf, sizeof(buf), "sq.z          VF24,%d(VI01)", PVK_VU1_INPUT_COUNT);
    vu(f, "NOP", buf, "the current triangle's sign");
    fprintf(f, "%s_loop:\n", sym);
    fprintf(f, "; indexed batch: the list holds each vertex's qword offset from the first\n");
    snprintf(buf, sizeof(buf), "ibeq          VI09,VI00,%s_fetch", sym);
    vu(f, "NOP", buf, "vertices in order: VI03 already points at the next one");
    vu(f, "NOP", "NOP", "branch delay slot");
    vu(f, "NOP", "ilw.x         VI08,0(VI06)", "index");
    vu(f, "NOP", "iaddiu        VI06,VI06,1", NULL);
    vu(f, "NOP", "iadd          VI03,VI10,VI08", "vertex it refers to");
    fprintf(f, "%s_fetch:\n", sym);
    fprintf(f, "; load inputs (location order; the pipeline repoints each at its binding's stream)\n");
    for (i = 0; i < sh->inputCount; i++) {
        snprintf(buf, sizeof(buf), "lq            VF%02d,%d(VI03)", 10 + i, i);
        snprintf(buf2, sizeof(buf2), "location %d", sh->inputs[i].location);
        vu(f, "NOP", buf, buf2);
    }

    fprintf(f, "; input conversion: the pipeline patches each input's 4 pairs for its attribute format\n");
    fprintf(f, "%s_Convert:\n", sym);
    snprintf(buf, sizeof(buf), "b             %s_body", sym);
    vu(f, "NOP", buf, "retargeted past the inputs that need nothing");
    vu(f, "NOP", "NOP", "branch delay slot");
    for (i = 0; i < sh->inputCount; i++) {
        snprintf(buf, sizeof(buf), "itof0.xyzw    VF%02d,VF%02d", 10 + i, 10 + i);
        vu(f, buf, "loi           0x3f800000", "integers to floats; I = UNORM/SNORM scale");
        snprintf(buf, sizeof(buf), "muli.xyzw     VF%02d,VF%02d,I", 10 + i, 10 + i);
        vu(f, buf, "loi           0xbf800000", "scale; I = SNORM clamp");
        snprintf(buf, sizeof(buf), "maxi.xyzw     VF%02d,VF%02d,I", 10 + i, 10 + i);
        vu(f, buf, "NOP", "clamp to -1");
        snprintf(buf, sizeof(buf), "mul.xyzw      VF%02d,VF00,VF00", 10 + i);
        vu(f, buf, "NOP", "components the format lacks: 0, w = 1");
    }
    fprintf(f, "%s_body:\n", sym);

    fprintf(f, "; shader body\n");
    for (i = 0; i < sh->bodyCount; i++) {
        const BodyOp *b = &sh->body[i];
        /* Shuffles and constructors read fields they have already written if the
         * result shares an operand's register, so they and the built-ins get a
         * distinct one. */
        int distinct = is_map_op(b->op) || b->op == OpDot || b->op == OpExtInst ||
                       b->op == OpSelect || b->op == OpBoolToFloat, d = 0;
        int regs[MAX_OPERANDS] = { 0 };
        char line[96];

        if (b->dead)
            continue;
        reg_sweep(&r, ids, i);
        if (b->line)
            snprintf(line, sizeof(line), "line %u: %s", (unsigned)b->line, body_name(b));
        else
            snprintf(line, sizeof(line), "%s", body_name(b));
        if (b->op == OpLabel) {
            if (targeted[b->result])
                fprintf(f, "%s_L%u:\n", sym, (unsigned)b->result);
            continue;
        }
        if (b->op == OpBranch) {
            if (targeted[b->target] && !(i + 1 < sh->bodyCount && sh->body[i + 1].op == OpLabel &&
                                         sh->body[i + 1].result == b->target)) {
                snprintf(buf, sizeof(buf), "b             %s_L%u", sym, (unsigned)b->target);
                vu(f, "NOP", buf, NULL);
                vu(f, "NOP", "NOP", "branch delay slot");
            }
            continue;
        }
        if (b->op == OpPhiMove) {
            /* The moves at the end of a block, as one parallel copy: a move whose
             * target another move still reads waits, a cycle goes through a temporary. */
            struct { int d, dc, s, sc, size; } mv[MAX_PHIS];
            int k = i, nm = 0, temps[MAX_PHIS], nt = 0;

            for (k = i; k < sh->bodyCount && sh->body[k].op == OpPhiMove; k++) {
                const BodyOp *m = &sh->body[k];
                uint32_t phi = m->result;
                reg_ensure_loaded(f, &r, ids, sh, m->ops[0]);
                if (!r.reg[phi])
                    r.reg[phi] = reg_alloc(&r);
                mv[nm].d = r.reg[phi];
                mv[nm].dc = ids[phi].comp;
                mv[nm].s = reg_of(&r, ids, sh, m->ops[0]);
                mv[nm].sc = ids[m->ops[0]].comp;
                mv[nm].size = type_size(ids, ids[phi].valType);
                nm++;
            }
            while (nm) {
                int m, o;
                const char *c = line;
                for (m = 0; m < nm; m++) {
                    for (o = 0; o < nm; o++)
                        if (o != m && mv[o].s == mv[m].d)
                            break;
                    if (o == nm)
                        break;
                }
                if (m == nm) {
                    int t = reg_alloc(&r);
                    temps[nt++] = t;
                    emit_l(f, &c, "move.xyzw", "VF%02d,VF%02d", t, mv[0].s);
                    mv[0].s = t;
                    continue;
                }
                if (mv[m].d != mv[m].s || (mv[m].size == 1 && mv[m].dc != mv[m].sc))
                    emit_move(f, &c, mv[m].d, mv[m].dc, mv[m].s, mv[m].sc, mv[m].size);
                mv[m] = mv[--nm];
            }
            for (int t = 0; t < nt; t++)
                r.busy[temps[t]] = 0;
            for (int j = i; j < k; j++) {
                reg_release(&r, ids, sh, sh->body[j].ops[0], j);
                reg_release(&r, ids, sh, sh->body[j].result, j);
            }
            i = k - 1;
            continue;
        }
        for (int j = 0; j < b->nops; j++)
            reg_ensure_loaded(f, &r, ids, sh, b->ops[j]);
        for (int j = 0; j < b->nops; j++)
            regs[j] = reg_of(&r, ids, sh, b->ops[j]);
        if (b->op == OpBranchConditional) {
            const char *c = line;
            int cur = 0, next = i + 1 < sh->bodyCount && sh->body[i + 1].op == OpLabel ?
                                (int)sh->body[i + 1].result : -1;
            d = reg_alloc(&r);
            emit_cond(f, &c, ids, regs, &cur, b->cond, d, sym);
            vu(f, "NOP", "NOP", "VI13 settles");
            if (next == (int)b->target) {
                emit_l(f, &c, "ibeq", "VI13,VI00,%s_L%u", sym, (unsigned)b->target2);
                vu(f, "NOP", "NOP", "branch delay slot");
            } else {
                emit_l(f, &c, "ibne", "VI13,VI00,%s_L%u", sym, (unsigned)b->target);
                vu(f, "NOP", "NOP", "branch delay slot");
                if (next != (int)b->target2) {
                    emit_l(f, &c, "b", "%s_L%u", sym, (unsigned)b->target2);
                    vu(f, "NOP", "NOP", "branch delay slot");
                }
            }
            r.busy[d] = 0;
            for (int j = 0; j < b->nops; j++)
                reg_release(&r, ids, sh, b->ops[j], i);
            continue;
        }
        if (distinct)
            d = r.reg[b->result] = reg_alloc(&r);
        for (int j = 0; j < b->nops; j++)
            reg_release(&r, ids, sh, b->ops[j], i);
        if (!distinct)
            d = r.reg[b->result] = reg_alloc(&r);
        emit_body_op(f, ids, sh, b, d, regs);
        reg_release(&r, ids, sh, b->result, i);   /* never read */
    }
    reg_sweep(&r, ids, sh->bodyCount);
    reg_ensure_loaded(f, &r, ids, sh, sh->positionValue);
    if (sh->colorValue)
        reg_ensure_loaded(f, &r, ids, sh, sh->colorValue);
    if (sh->texcoordValue)
        reg_ensure_loaded(f, &r, ids, sh, sh->texcoordValue);
    snprintf(buf, sizeof(buf), "move.xyzw     VF20,VF%02d", reg_of(&r, ids, sh, sh->positionValue));
    vu(f, "NOP", buf, "gl_Position");
    if (sh->colorValue) {
        snprintf(buf, sizeof(buf), "move.xyzw     VF21,VF%02d", reg_of(&r, ids, sh, sh->colorValue));
        snprintf(buf2, sizeof(buf2), "color output, location %d", sh->colorLocation);
        vu(f, "NOP", buf, buf2);
    } else {
        vu(f, "NOP", "move.xyzw     VF21,VF00", "no color output: black");
    }
    if (sh->texcoordValue) {
        snprintf(buf, sizeof(buf), "move.xy       VF%02d,VF%02d", PVK_VU1_TEXCOORD_REG,
                 reg_of(&r, ids, sh, sh->texcoordValue));
        snprintf(buf2, sizeof(buf2), "texture coordinate output, location %d", sh->texcoordLocation);
        vu(f, "NOP", buf, buf2);
    }

    fprintf(f, "; epilogue: perspective divide, viewport, GS formats, store\n");
    if (sh->texcoordValue) {
        snprintf(buf, sizeof(buf), "ilw.w         VI13,%d(VI01)", PVK_VU1_INPUT_COUNT);
        vu(f, "NOP", buf, "1 = UV in texels (PRIM.FST)");
    }
    vu(f, "NOP", "div           Q,VF00w,VF20w", "Q = 1 / w");
    vu(f, "NOP", "waitq", NULL);
    vu(f, "mulq.xyz      VF22,VF20,Q", "NOP", NULL);
    if (sh->texcoordValue) {
        /* The GS interpolates S/w, T/w and Q = 1/w and divides per pixel, which
         * makes the texture perspective correct. With FST it takes U, V in
         * 12.4 texels as is. */
        snprintf(buf, sizeof(buf), "ibne          VI13,VI00,%s_uv", sym);
        vu(f, "NOP", buf, NULL);
        vu(f, "NOP", "NOP", "branch delay slot");
        snprintf(buf, sizeof(buf), "mulq.xy       VF%02d,VF%02d,Q", PVK_VU1_TEXCOORD_REG, PVK_VU1_TEXCOORD_REG);
        vu(f, buf, "NOP", "S / w, T / w");
        snprintf(buf, sizeof(buf), "b             %s_st", sym);
        vu(f, "NOP", buf, NULL);
        snprintf(buf, sizeof(buf), "addq.z        VF%02d,VF00,Q", PVK_VU1_TEXCOORD_REG);
        vu(f, buf, "NOP", "delay slot: Q = 1 / w");
        fprintf(f, "%s_uv:\n", sym);
        snprintf(buf, sizeof(buf), "ftoi4.xy      VF%02d,VF%02d", PVK_VU1_TEXCOORD_REG, PVK_VU1_TEXCOORD_REG);
        vu(f, buf, "NOP", "U, V to 12.4 fixed point");
        fprintf(f, "%s_st:\n", sym);
    }
    vu(f, "mul.xyz       VF22,VF22,VF01", "NOP", "viewport scale");
    vu(f, "add.xyz       VF22,VF22,VF02", "NOP", "viewport offset");
    vu(f, "ftoi4.xy      VF23,VF22", "NOP", "x, y to 12.4 fixed point");
    vu(f, "ftoi0.z       VF23,VF22", "NOP", "z to integer depth");
    vu(f, "mul.xyzw      VF24,VF21,VF03", "NOP", "color 0..1 to GS range");
    vu(f, "ftoi0.xyzw    VF24,VF24", "NOP", NULL);
    if (sh->texcoordValue) {
        snprintf(buf, sizeof(buf), "sq            VF%02d,0(VI05)", PVK_VU1_TEXCOORD_REG);
        vu(f, "NOP", buf, "ST (and Q), or UV");
        vu(f, "NOP", "sq            VF24,1(VI05)", "RGBAQ");
    } else {
        vu(f, "NOP", "sq            VF24,0(VI05)", "RGBAQ");
    }
    emit_cull(f, sym);
    if (sh->texcoordValue) {
        vu(f, "NOP", "sq            VF23,2(VI05)", "XYZ2 (draws unless ADC is set)");
        snprintf(buf, sizeof(buf), "iaddiu        VI05,VI05,%d", PVK_VU1_OUTPUT_PER_VERTEX_TEXTURED);
    } else {
        vu(f, "NOP", "sq            VF23,1(VI05)", "XYZ2 (draws unless ADC is set)");
        snprintf(buf, sizeof(buf), "iaddiu        VI05,VI05,%d", PVK_VU1_OUTPUT_PER_VERTEX);
    }
    vu(f, "NOP", buf, NULL);
    vu(f, "NOP", "iadd          VI03,VI03,VI07", "next vertex");
    vu(f, "NOP", "isubiu        VI02,VI02,1", NULL);
    snprintf(buf, sizeof(buf), "ilw.y         VI08,%d(VI01)", PVK_VU1_INPUT_STRIDE);
    vu(f, "NOP", buf, "qwords per instance");
    snprintf(buf, sizeof(buf), "ibne          VI02,VI00,%s_loop", sym);
    vu(f, "NOP", buf, NULL);
    snprintf(buf, sizeof(buf), "lq            VF24,%d(VI01)", PVK_VU1_INPUT_STRIDE);
    vu(f, "NOP", buf, "delay slot: z = gl_InstanceIndex");
    vu(f, "NOP", "iadd          VI15,VI15,VI08", "next instance");
    vu(f, "addw.z        VF24,VF24,VF00", "NOP", "gl_InstanceIndex + 1");
    snprintf(buf, sizeof(buf), "sq.z          VF24,%d(VI01)", PVK_VU1_INPUT_STRIDE);
    vu(f, "NOP", buf, NULL);
    snprintf(buf, sizeof(buf), "ibne          VI14,VI00,%s_instance", sym);
    vu(f, "NOP", buf, NULL);
    vu(f, "NOP", "NOP", "branch delay slot");

    fprintf(f, "; send the batch to the GS and stop\n");
    vu(f, "NOP", "xgkick        VI04", NULL);
    vu(f, "NOP[E]", "NOP", NULL);
    vu(f, "NOP", "NOP", NULL);
    fprintf(f, "\t\t.align 4\n%s_CodeEnd:\n", sym);
    for (i = 0; i < g_clamps; i++)
        fprintf(f, "\t\t.global\t%s_Clamp%d\n", sym, i);
    fclose(f);

    f = open_out(base, ".c");
    fprintf(f, "/* Generated by ps2vk-shaderc from %s. Do not edit. */\n", g_src);
    fprintf(f, "#include \"ps2vk.h\"\n\n");
    fprintf(f, "extern uint32_t %s_CodeStart __attribute__((section(\".vudata\")));\n", sym);
    fprintf(f, "extern uint32_t %s_Convert __attribute__((section(\".vudata\")));\n", sym);
    fprintf(f, "extern uint32_t %s_CodeEnd __attribute__((section(\".vudata\")));\n", sym);
    for (i = 0; i < g_clamps; i++)
        fprintf(f, "extern uint32_t %s_Clamp%d __attribute__((section(\".vudata\")));\n", sym, i);
    if (g_clamps) {
        fprintf(f, "\nstatic const uint32_t *const %s_Clamps[] = {", sym);
        for (i = 0; i < g_clamps; i++)
            fprintf(f, "%s&%s_Clamp%d", i ? ", " : " ", sym, i);
        fprintf(f, " };\n");
    }
    fprintf(f, "\nconst PvkShaderCode %s = {\n", sym);
    fprintf(f, "    .stage = PVK_SHADER_STAGE_VERTEX,\n");
    fprintf(f, "    .name = \"%s\",\n", g_src);
    fprintf(f, "    .vu1CodeStart = &%s_CodeStart,\n", sym);
    fprintf(f, "    .vu1CodeEnd = &%s_CodeEnd,\n", sym);
    fprintf(f, "    .vu1Convert = &%s_Convert,\n", sym);
    if (g_clamps) {
        fprintf(f, "    .vu1ClampCount = %d,\n", g_clamps);
        fprintf(f, "    .vu1Clamps = %s_Clamps,\n", sym);
    }
    fprintf(f, "    .inputCount = %d,\n", sh->inputCount);
    fprintf(f, "    .inputLocations = {");
    for (i = 0; i < sh->inputCount; i++)
        fprintf(f, "%s%d", i ? ", " : " ", sh->inputs[i].location);
    fprintf(f, " },\n");
    fprintf(f, "    .colorOutputLocation = %d,\n", sh->colorValue ? sh->colorLocation : -1);
    fprintf(f, "    .texcoordOutputLocation = %d,\n", sh->texcoordValue ? sh->texcoordLocation : -1);
    fprintf(f, "    .pushConstantBytes = %u,\n", (unsigned)sh->pushBytes);
    fprintf(f, "    .uniformBlockCount = %d,\n", sh->uniformCount);
    if (sh->uniformCount) {
        fprintf(f, "    .uniformBlocks = {\n");
        for (i = 0; i < sh->uniformCount; i++)
            fprintf(f, "        { .set = %d, .binding = %d, .qword = %u, .bytes = %u }, /* %s */\n",
                    sh->uniforms[i].set, sh->uniforms[i].binding, (unsigned)sh->uniforms[i].qword,
                    (unsigned)sh->uniforms[i].bytes, sh->uniforms[i].name);
        fprintf(f, "    },\n");
    }
    fprintf(f, "    .colorInputLocation = -1,\n");
    fprintf(f, "    .texcoordInputLocation = -1,\n");
    fprintf(f, "};\n");
    fclose(f);
}

static void emit_fragment(const Shader *sh, const IdInfo *ids, const char *base, const char *sym)
{
    FILE *f = open_out(base, ".c");
    int texel = ids[sh->colorValue].texel;
    int usesColor = texel == TEXEL_NONE || texel == TEXEL_SAMPLE_TIMES_COLOR;
    int usesTexture = texel != TEXEL_NONE;

    fprintf(f, "/* Generated by ps2vk-shaderc from %s. Do not edit. */\n", g_src);
    fprintf(f, "#include \"ps2vk.h\"\n\n");
    if (texel == TEXEL_NONE)
        fprintf(f, "/* outColor = interpolated color: Gouraud shading, no texture. */\n");
    else if (texel == TEXEL_SAMPLE)
        fprintf(f, "/* outColor = texture sample: GS decal, the vertex color is ignored. */\n");
    else
        fprintf(f, "/* outColor = texture sample * interpolated color: GS modulate. */\n");
    fprintf(f, "const PvkShaderCode %s = {\n", sym);
    fprintf(f, "    .stage = PVK_SHADER_STAGE_FRAGMENT,\n");
    fprintf(f, "    .name = \"%s\",\n", g_src);
    fprintf(f, "    .colorOutputLocation = -1,\n");
    fprintf(f, "    .texcoordOutputLocation = -1,\n");
    fprintf(f, "    .colorInputLocation = %d,\n",
            usesColor && sh->colorInput >= 0 ? sh->inputs[sh->colorInput].location : -1);
    fprintf(f, "    .texcoordInputLocation = %d,\n",
            usesTexture ? sh->inputs[sh->texcoordInput].location : -1);
    fprintf(f, "    .gouraud = %d,\n", usesColor ? 1 : 0);
    fprintf(f, "    .textured = %d,\n", usesTexture ? 1 : 0);
    fprintf(f, "    .textureFunction = %d,\n", usesTexture ? sh->textureFunction : 0);
    fprintf(f, "    .samplerBinding = %d,\n", usesTexture ? sh->samplerBinding : -1);
    fprintf(f, "    .samplerSet = %d,\n", usesTexture ? sh->samplerSet : -1);
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

    {
        const char *opt = optimize(in, outBase);
        code = read_spirv(opt, &words);
        remove(opt);
    }
    parse(code, words, ids, &sh);

    if (sh.execModel == ExecVertex)
        emit_vertex(&sh, ids, outBase, sym);
    else
        emit_fragment(&sh, ids, outBase, sym);

    free(code);
    return 0;
}
