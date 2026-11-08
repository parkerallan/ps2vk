/*
 * ps2vk-shaderc: compiles SPIR-V (glslangValidator -V) for the PS2.
 *
 *   .vert --glslangValidator--> .spv --shaderc--> .vsm + .c --dvp-as--> .o + .c  (VU1 microcode + PvkShaderCode)
 *   .frag --glslangValidator--> .spv --shaderc--> .c                            (GS settings in a PvkShaderCode)
 *
 * This version accepts:
 *
 *   vertex:   gl_Position, an optional vec4 color output and an optional vec2
 *             texture coordinate output computed from vec4 inputs, push
 *             constants (a block of float/vec2/vec3/vec4/matrix members and
 *             arrays of vectors, up to 128 bytes) and float/vec4/matrix constants
 *             with +, -, *, / and negation, matrix * vector, swizzles (v.xyz,
 *             v.bgra), component reads (v.w), vector constructors and math
 *             built-ins
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

#include "ps2vk_vu1_layout.h"

#define MAX_IDS     4096
#define MAX_INPUTS  8
#define MAX_BODY    256
#define MAX_OPERANDS 5

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
    OpCompositeExtract = 81, OpTranspose = 84, OpSampledImage = 86,
    OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88,
    OpFNegate = 127, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136,
    OpVectorTimesScalar = 142, OpMatrixTimesScalar = 143, OpVectorTimesMatrix = 144,
    OpMatrixTimesVector = 145, OpMatrixTimesMatrix = 146, OpOuterProduct = 147, OpDot = 148,
    OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249,
    OpBranchConditional = 250, OpKill = 252, OpReturn = 253, OpReturnValue = 254,
    OpNoLine = 317, OpModuleProcessed = 330
};

enum { DecBlock = 2, DecRowMajor = 4, DecColMajor = 5, DecArrayStride = 6, DecMatrixStride = 7,
       DecBuiltIn = 11, DecLocation = 30, DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35 };
enum { BuiltInPosition = 0 };
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
               T_SAMPLER /* sampler2D: OpTypeSampledImage */ } TypeKind;

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
    /* variables and pointers */
    uint32_t varType;        /* pointer type id */
    int      location;       /* -1 if none */
    int      binding;        /* -1 if none */
    int      set;            /* descriptor set, -1 if none */
    int      builtin;        /* -1 if none */
    int      structPosMember;/* gl_PerVertex: member index decorated Position */
    int      isPosition;     /* pointer that targets gl_Position */
    /* values: which input or push constant qword a loaded value came from */
    int      fromInput;      /* index into inputs[], or -1 */
    int      fromPush;       /* push constant qword (0..PVK_VU1_PUSH_QWORDS-1), or -1 */
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
    case OpLoad: return "OpLoad (a push constant read)";
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
             "Only 'outColor = <color input>;', 'outColor = texture(<sampler>, <texcoord input>);' "
             "and their product are supported so far", what);
    fail("vertex shader uses %s, which this version of ps2vk-shaderc does not "
         "compile yet", what);
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

/* Component count of a float or vecN type, 0 for anything else. */
static int type_size(const IdInfo *ids, uint32_t type)
{
    const IdInfo *t = &ids[type];
    if (is_float(ids, type))
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
    uint32_t line;                  /* GLSL line, 0 if unknown */
} BodyOp;

enum { FIELD_ZERO = -1, FIELD_ONE = -2, FIELD_UNDEF = -3 };
#define SHUFFLE_UNDEF 0xffffffffu

static int is_map_op(uint32_t op)
{
    return op == OpVectorShuffle || op == OpCompositeConstruct || op == OpCompositeExtract ||
           op == OpLoad;   /* a vec2/vec3 push constant that does not start at field x */
}

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
    uint32_t pushBytes;             /* size of the push constant block */
    uint32_t pushBase;              /* synthetic id of push constant qword 0 (then qword 1, ...) */
    int      pushUsed[PVK_VU1_PUSH_QWORDS]; /* qwords the shader reads */
    uint32_t positionVar;           /* gl_PerVertex or Position variable, 0 if none */
    uint32_t positionValue;         /* value id stored to gl_Position, 0 if never */
    uint32_t colorValue;            /* value id stored to the color output, 0 if never */
    int      colorLocation;         /* -1 if none */
    BodyOp   body[MAX_BODY];
    int      bodyCount;
} Shader;

/* Where component j of value id really lives: (source id, field), seen through
 * aliases and earlier maps. */
typedef struct { uint32_t src; int comp; } Field;

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
    if (ids[id].aliasOf)
        return field_of(ids, sh, ids[id].aliasOf, ids[id].comp + j);
    /* Every load of an input (or push constant qword) reads the same register:
     * name it by the variable so in.x and in.y loaded separately still group
     * into one MOVE. */
    if (ids[id].kind == ID_VALUE && ids[id].fromInput >= 0)
        fl.src = sh->inputs[ids[id].fromInput].var;
    else if (ids[id].kind == ID_VALUE && ids[id].fromPush >= 0)
        fl.src = sh->pushBase + (uint32_t)ids[id].fromPush;
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
        fail("corrupt SPIR-V: push constant member %u has no Offset decoration", (unsigned)member);
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

/* Checks a push constant block: members are float, vec2, vec3, vec4 or arrays of
 * those, and the block fits the VU1 range. Returns its size in bytes. */
static uint32_t check_push_block(const IdInfo *ids, uint32_t structType, const char *name)
{
    const IdInfo *s = &ids[structType];
    uint32_t bytes = 0;

    if (s->type != T_STRUCT)
        fail("push constant '%s' is not a block; declare it as "
             "'layout(push_constant) uniform Name { ... } %s;'", name, name);
    for (int m = 0; m < s->memberCount; m++) {
        uint32_t t = s->members[m], off = member_offset(structType, (uint32_t)m), end;
        if (ids[t].type == T_ARRAY) {
            if (!type_size(ids, ids[t].elemType))
                fail("member %d of push constant block '%s' is an array of something other than "
                     "float, vec2, vec3 or vec4, which is not supported yet%s", m, name,
                     matrix_cols(ids, ids[t].elemType) ? " (arrays of matrices come with skinning)" : "");
            end = off + ids[t].stride * ids[t].elemCount;
        } else if (type_size(ids, t)) {
            end = off + 4 * (uint32_t)type_size(ids, t);
        } else if (matrix_cols(ids, t)) {
            /* VU1 reads a column per qword and broadcasts the vector's fields over
             * them, so the columns must be the qwords: column-major, 16 bytes apart
             * (what std430 gives every matCxR). */
            const MemberDecor *d = member_decor(structType, (uint32_t)m, 0);
            if (d->rowMajor)
                fail("member %d of push constant block '%s' is a row_major matrix; VU1 "
                     "multiplies by columns, so leave the block column-major (the default)",
                     m, name);
            if (d->matrixStride != 16)
                fail("corrupt SPIR-V: matrix member %d of push constant block '%s' has a "
                     "column stride of %u bytes, not 16", m, name, (unsigned)d->matrixStride);
            if (off % 16)
                fail("member %d of push constant block '%s' is a matrix at byte %u, which is "
                     "not 16-byte aligned", m, name, (unsigned)off);
            end = off + 16 * (uint32_t)matrix_cols(ids, t);
        } else if (ids[t].type == T_INT && ids[t].width == 32) {
            end = off + 4;   /* may be declared; using it in arithmetic is rejected */
        } else {
            fail("member %d of push constant block '%s' is something other than float, "
                 "vec2, vec3, vec4, a matrix, int or an array of vectors (arrays of "
                 "matrices and nested blocks are not supported yet)", m, name);
        }
        if (end > bytes)
            bytes = end;
    }
    if (bytes > PVK_VU1_PUSH_QWORDS * 16)
        fail("push constant block '%s' is %u bytes; ps2vk reserves %d bytes of VU1 memory "
             "for push constants", name, (unsigned)bytes, PVK_VU1_PUSH_QWORDS * 16);
    return bytes;
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

static void parse(const uint32_t *code, size_t words, IdInfo *ids, Shader *sh)
{
    size_t pc;
    uint32_t bound = code[3];
    int inFunction = 0, functions = 0;

    /* Ids past the SPIR-V bound stand for the push constant qwords. */
    if (bound + PVK_VU1_PUSH_QWORDS > MAX_IDS)
        fail("shader is too large (%u ids)", (unsigned)bound);

    memset(sh, 0, sizeof(*sh));
    sh->execModel = ~0u;
    sh->colorLocation = sh->texcoordLocation = -1;
    sh->colorInput = sh->texcoordInput = sh->samplerBinding = sh->samplerSet = -1;
    sh->pushBase = bound;
    for (uint32_t i = 0; i < bound + PVK_VU1_PUSH_QWORDS; i++) {
        ids[i].location = ids[i].binding = ids[i].set = ids[i].builtin = ids[i].structPosMember = -1;
        ids[i].fromInput = ids[i].fromPush = -1;
    }
    for (int q = 0; q < PVK_VU1_PUSH_QWORDS; q++) {
        ids[sh->pushBase + q].kind = ID_VALUE;
        ids[sh->pushBase + q].fromPush = q;
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
        case OpTypeVoid: case OpTypeBool: case OpTypeRuntimeArray:
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
            if (!inFunction && storage == ScUniformConstant && ids[ids[type].elemType].type == T_SAMPLER) {
                if (sh->execModel != ExecFragment)
                    fail("vertex shader declares sampler '%s': VU1 cannot read textures. "
                         "Sample in the fragment shader instead",
                         ids[id].name ? ids[id].name : "variable");
            } else if (!inFunction && storage == ScUniformConstant) {
                fail("'%s' is a separate texture or sampler; the GS binds both together, "
                     "so declare a sampler2D", ids[id].name ? ids[id].name : "variable");
            } else if (!inFunction && storage != ScInput && storage != ScOutput && storage != ScPushConstant) {
                fail("'%s' is a uniform or buffer; this version of ps2vk-shaderc supports "
                     "only shader inputs, outputs, push constants and a sampler2D (uniform "
                     "blocks need pipeline layouts, which are not implemented yet)",
                     ids[id].name ? ids[id].name : "variable");
            }
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
                sh->pushBytes = check_push_block(ids, pointee, name);
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
                if (ids[ptr].kind == ID_PTR && ids[ptr].fromPush >= 0) {
                    /* A push constant member: its qword is loaded into a register in
                     * the prologue, so this reads that register in place... */
                    int size = type_size(ids, w[1]);
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    if (matrix_cols(ids, w[1])) {
                        /* A whole matrix: its columns are the qwords from fromPush on. */
                        ids[id].fromPush = ids[ptr].fromPush;
                        break;
                    }
                    if (size > 1 && ids[ptr].comp) {
                        /* ...unless it is a vec2/vec3 past field x (vec2 at offset 8):
                         * arithmetic expects vectors in the leading fields, so copy it. */
                        Field fl[4];
                        for (int j = 0; j < size; j++) {
                            fl[j].src = sh->pushBase + (uint32_t)ids[ptr].fromPush;
                            fl[j].comp = ids[ptr].comp + j;
                        }
                        define_map(ids, sh, OpLoad, id, w[1], fl, size);
                        break;
                    }
                    ids[id].fromPush = ids[ptr].fromPush;
                    ids[id].comp = ids[ptr].comp;
                    break;
                }
                if (ptr && ptr == sh->samplerVar) {
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].texel = TEXEL_SAMPLER;
                    break;
                }
                if ((ids[ptr].kind != ID_VAR && ids[ptr].kind != ID_PTR) || ids[ptr].fromInput < 0)
                    fail("%s shader reads something other than a vertex input or push constant", stage);
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
            case OpFAdd: case OpFSub: case OpFMul: case OpFDiv: case OpFNegate:
            case OpVectorTimesScalar: {
                BodyOp *b;
                int size;

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
                    if (value_size(ids, w[3], stage) != size || value_size(ids, w[4], stage) != 1)
                        fail("corrupt SPIR-V: OpVectorTimesScalar needs a vector and a float");
                } else if (value_size(ids, w[3], stage) != size ||
                           (op != OpFNegate && value_size(ids, w[4], stage) != size)) {
                    fail("corrupt SPIR-V: %s operands differ in size", op_name(op));
                }
                /* A scalar result stays in the field its first operand uses; a division
                 * is copied out of Q into x. */
                if (size == 1)
                    ids[w[2]].comp = op == OpFDiv ? 0 : ids[w[3]].comp;
                if (sh->bodyCount == MAX_BODY)
                    fail("vertex shader is too long (max %d instructions)", MAX_BODY);
                b = &sh->body[sh->bodyCount++];
                b->op = op;
                b->result = w[2];
                b->ops[0] = w[3];
                b->nops = 1;
                if (op != OpFNegate)
                    b->ops[b->nops++] = w[4];
                b->line = g_line;
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
                } else if (ids[m].kind == ID_VALUE && ids[m].fromPush >= 0) {
                    for (int c = 0; c < cols; c++)
                        b->ops[c] = sh->pushBase + (uint32_t)(ids[m].fromPush + c);
                } else {
                    fail("vertex shader multiplies by a matrix that is neither a push constant "
                         "nor a constant; only those are supported so far");
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
                         "only push constant and constant matrices are supported so far");
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
                uint32_t id = w[2], base = w[3];
                if (base && base == sh->pushVar) {
                    /* pc.member, pc.array[i], pc.member.z: a constant byte offset into
                     * the push constant range, which VU1 memory holds as qwords. */
                    uint32_t type = ids[ids[base].varType].elemType, off = 0;
                    int size;

                    for (uint32_t i = 4; i < n; i++) {
                        uint32_t idx = w[i], v;
                        if (ids[idx].kind != ID_CONST)
                            fail("vertex shader indexes push constants with a value computed at "
                                 "run time; only constant indices are supported so far");
                        v = ids[idx].value;
                        if (ids[type].type == T_STRUCT) {
                            if (v >= (uint32_t)ids[type].memberCount)
                                fail("corrupt SPIR-V: push constant member index out of range");
                            off += member_offset(type, v);
                            type = ids[type].members[v];
                        } else if (ids[type].type == T_ARRAY) {
                            if (v >= ids[type].elemCount)
                                fail("vertex shader reads element %u of a push constant array "
                                     "of %u", (unsigned)v, (unsigned)ids[type].elemCount);
                            off += ids[type].stride * v;
                            type = ids[type].elemType;
                        } else if (ids[type].type == T_MATRIX) {
                            /* pc.m[c]: column c is the c-th qword of the matrix. */
                            if (v >= ids[type].elemCount)
                                fail("vertex shader reads column %u of a push constant matrix "
                                     "with %u columns", (unsigned)v, (unsigned)ids[type].elemCount);
                            off += 16 * v;
                            type = ids[type].elemType;
                        } else if (ids[type].type == T_VECTOR) {
                            if (v >= ids[type].elemCount)
                                fail("corrupt SPIR-V: push constant component index out of range");
                            off += 4 * v;
                            type = ids[type].elemType;
                        } else {
                            fail("corrupt SPIR-V: push constant access chain indexes a scalar");
                        }
                    }
                    if (matrix_cols(ids, type)) {
                        /* pc.m as a whole, for matrix * vector: every column's qword is used. */
                        int cols = matrix_cols(ids, type);
                        if (off + 16 * (uint32_t)cols > sh->pushBytes)
                            fail("corrupt SPIR-V: push constant read past the end of the block");
                        ids[id].kind = ID_PTR;
                        ids[id].fromPush = (int)(off / 16);
                        for (int c = 0; c < cols; c++)
                            sh->pushUsed[off / 16 + (uint32_t)c] = 1;
                        break;
                    }
                    size = type_size(ids, type);
                    if (!size && ids[type].type == T_INT)
                        size = 1;   /* loadable; any arithmetic on it is rejected */
                    if (!size)
                        fail("vertex shader reads a whole push constant block or array; "
                             "read its float, vec2, vec3 or vec4 members one at a time");
                    if (off % 16 + 4 * (uint32_t)size > 16)
                        fail("push constant member at byte %u crosses a 16-byte boundary, "
                             "which VU1 cannot read as one value", (unsigned)off);
                    if (off + 4 * (uint32_t)size > sh->pushBytes)
                        fail("corrupt SPIR-V: push constant read past the end of the block");
                    ids[id].kind = ID_PTR;
                    ids[id].fromPush = (int)(off / 16);
                    ids[id].comp = (int)(off % 16 / 4);
                    sh->pushUsed[off / 16] = 1;
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
                if (ids[ptr].isPosition) {
                    if (value_size(ids, val, stage) != 4)
                        fail("vertex shader stores something other than a vec4 to gl_Position");
                    sh->positionValue = val;
                } else if (ptr && ptr == sh->colorVar) {
                    if (value_size(ids, val, stage) != 4)
                        fail("vertex shader stores something other than a vec4 to the color output");
                    sh->colorValue = val;
                } else if (ptr && ptr == sh->texcoordVar) {
                    if (value_size(ids, val, stage) != 2)
                        fail("vertex shader stores something other than a vec2 to the texture "
                             "coordinate output");
                    sh->texcoordValue = val;
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

/*
 * VI08 = VI07 * qwords, the VU1 memory offset of an indexed vertex. VU1's
 * integer unit has no multiply or shift, so VI07 is doubled for each bit of
 * the count and the set bits are summed; at most 6 instructions for 8 inputs.
 */
static void emit_index_scale(FILE *f, uint32_t qwords)
{
    int first = 1;
    char buf[64];

    for (uint32_t bit = 1; bit <= qwords; bit <<= 1) {
        if (qwords & bit) {
            snprintf(buf, sizeof(buf), "iadd          VI08,VI%s,VI07", first ? "00" : "08");
            vu(f, "NOP", buf, first ? "index * qwords per vertex" : NULL);
            first = 0;
        }
        if ((bit << 1) <= qwords)
            vu(f, "NOP", "iadd          VI07,VI07,VI07", NULL);
    }
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
    id = root(ids, id);
    if (ids[id].kind == ID_VALUE && ids[id].fromInput < 0 && ids[id].fromPush < 0 &&
        r->lastUse[id] <= at && r->reg[id]) {
        r->busy[r->reg[id]] = 0;
        r->reg[id] = 0;
    }
}

/* Register holding a value (an alias reads its root's register). */
static int reg_of(const Regs *r, const IdInfo *ids, uint32_t id)
{
    return r->reg[root(ids, id)];
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
    if (b->op == OpDot)
        return "dot()";
    return op_name(b->op);
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
static void emit_body_op(FILE *f, const IdInfo *ids, const BodyOp *b, int d, const int *regs)
{
    static const char field[] = "xyzw";
    int size = type_size(ids, ids[b->result].valType);
    int a = regs[0], bb = regs[1];
    int cols = b->nops - 1, vreg = regs[cols];   /* matrix * vector */
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
    case OpVectorShuffle: case OpCompositeConstruct: case OpCompositeExtract:
        emit_map(f, b, d, regs, size, line);
        break;
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
    int i, constants = 0;

    /* Registers: inputs are fixed, constants live for the whole program, temporaries
     * are freed after their last read. */
    memset(&r, 0, sizeof(r));
    for (i = 0; i < sh->bodyCount; i++)
        for (int j = 0; j < sh->body[i].nops && !sh->body[i].dead; j++)
            r.lastUse[root(ids, sh->body[i].ops[j])] = i;
    r.lastUse[root(ids, sh->positionValue)] = MAX_BODY;
    if (sh->colorValue)
        r.lastUse[root(ids, sh->colorValue)] = MAX_BODY;
    if (sh->texcoordValue) {
        r.lastUse[root(ids, sh->texcoordValue)] = MAX_BODY;
        r.busy[PVK_VU1_TEXCOORD_REG] = 1;   /* the STQ output lives there */
    }
    for (i = 0; i < MAX_IDS; i++)
        if ((ids[i].kind == ID_VALUE || ids[i].kind == ID_VAR) && ids[i].fromInput >= 0)
            r.reg[i] = 10 + ids[i].fromInput;
    /* Push constant qwords the shader reads stay in registers for the whole program. */
    for (i = 0; i < PVK_VU1_PUSH_QWORDS; i++)
        if (sh->pushUsed[i])
            r.reg[sh->pushBase + i] = reg_alloc(&r);
    for (i = 0; i < MAX_IDS; i++)
        if (ids[i].kind == ID_VALUE && ids[i].fromPush >= 0)
            r.reg[i] = r.reg[sh->pushBase + ids[i].fromPush];

    fprintf(f, "; Generated by ps2vk-shaderc from %s. Do not edit.\n", g_src);
    fprintf(f, ";\n; VU1 registers:\n");
    fprintf(f, ";   VF01 viewport scale, VF02 viewport offset, VF03 color scale\n");
    fprintf(f, ";   VF10.. shader inputs, VF20 gl_Position, VF21 color output\n");
    if (sh->texcoordValue)
        fprintf(f, ";   VF%02d texture coordinate output (S, T, Q)\n", PVK_VU1_TEXCOORD_REG);
    fprintf(f, ";   VF05-VF09, VF18-VF19, VF25-VF31 push constants, constants and temporaries\n");
    fprintf(f, ";   VI02 vertices left, VI03 input pointer, VI04 output base, VI05 output pointer\n");
    fprintf(f, ";   VI06 index pointer, VI07-VI08 index scaling, VI09 index list offset (0 = in order),\n");
    fprintf(f, ";   VI10 first vertex\n\n");
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
    for (i = 0; i < PVK_VU1_PUSH_QWORDS; i++) {
        if (!sh->pushUsed[i])
            continue;
        snprintf(buf, sizeof(buf), "lq            VF%02d,%d(VI00)", r.reg[sh->pushBase + i], PVK_VU1_PUSH_ADDR + i);
        snprintf(buf2, sizeof(buf2), "push constants, bytes %d..%d", i * 16, i * 16 + 15);
        vu(f, "NOP", buf, buf2);
    }
    snprintf(buf, sizeof(buf), "iaddiu        VI01,VI00,%d", PVK_VU1_INPUT_ADDR);
    vu(f, "NOP", buf, "input base");
    snprintf(buf, sizeof(buf), "lq            VF04,%d(VI01)", PVK_VU1_INPUT_GIFTAG);
    vu(f, "NOP", buf, "GIF tag for this batch");
    snprintf(buf, sizeof(buf), "ilw.x         VI02,%d(VI01)", PVK_VU1_INPUT_COUNT);
    vu(f, "NOP", buf, "vertex count");
    snprintf(buf, sizeof(buf), "iaddiu        VI03,VI01,%d", PVK_VU1_INPUT_HEADER);
    vu(f, "NOP", buf, "first vertex");
    snprintf(buf, sizeof(buf), "ilw.y         VI09,%d(VI01)", PVK_VU1_INPUT_COUNT);
    vu(f, "NOP", buf, "index list offset, 0 = vertices in order");
    vu(f, "NOP", "iadd          VI10,VI03,VI00", "first vertex, kept for indexed fetches");
    vu(f, "NOP", "iadd          VI06,VI01,VI09", "index pointer");
    snprintf(buf, sizeof(buf), "iaddiu        VI04,VI00,%d", PVK_VU1_OUTPUT_ADDR);
    vu(f, "NOP", buf, "output base");
    vu(f, "sub.xyzw      VF23,VF00,VF00", "sq            VF04,0(VI04)", "XYZ2 w = 0 (ADC off)");
    vu(f, "NOP", "iaddiu        VI05,VI04,1", "output pointer");

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
            uint32_t c = use[j] ? root(ids, use[j]) : 0;
            if (!c || ids[c].kind != ID_CONST || r.reg[c])
                continue;
            if (!constants++)
                fprintf(f, "; constants\n");
            r.reg[c] = reg_alloc(&r);
            emit_constant(f, &ids[c], r.reg[c]);
        }
    }

    fprintf(f, "%s_loop:\n", sym);
    fprintf(f, "; indexed batch: the next vertex is first vertex + index * %u\n", (unsigned)sh->inputCount);
    snprintf(buf, sizeof(buf), "ibeq          VI09,VI00,%s_fetch", sym);
    vu(f, "NOP", buf, "vertices in order: VI03 already points at the next one");
    vu(f, "NOP", "NOP", "branch delay slot");
    vu(f, "NOP", "ilw.x         VI07,0(VI06)", "index");
    vu(f, "NOP", "iaddiu        VI06,VI06,1", NULL);
    emit_index_scale(f, sh->inputCount);
    vu(f, "NOP", "iadd          VI03,VI10,VI08", "vertex it refers to");
    fprintf(f, "%s_fetch:\n", sym);
    fprintf(f, "; load inputs (location order)\n");
    for (i = 0; i < sh->inputCount; i++) {
        snprintf(buf, sizeof(buf), "lq            VF%02d,%d(VI03)", 10 + i, i);
        snprintf(buf2, sizeof(buf2), "location %d", sh->inputs[i].location);
        vu(f, "NOP", buf, buf2);
    }

    fprintf(f, "; shader body\n");
    for (i = 0; i < sh->bodyCount; i++) {
        const BodyOp *b = &sh->body[i];
        /* Shuffles and constructors read fields they have already written if the
         * result shares an operand's register, so they and the built-ins get a
         * distinct one. */
        int distinct = is_map_op(b->op) || b->op == OpDot || b->op == OpExtInst, d = 0;
        int regs[MAX_OPERANDS] = { 0 };

        if (b->dead)
            continue;
        for (int j = 0; j < b->nops; j++)
            regs[j] = reg_of(&r, ids, b->ops[j]);
        if (distinct)
            d = r.reg[b->result] = reg_alloc(&r);
        for (int j = 0; j < b->nops; j++)
            reg_release(&r, ids, b->ops[j], i);
        if (!distinct)
            d = r.reg[b->result] = reg_alloc(&r);
        emit_body_op(f, ids, b, d, regs);
        reg_release(&r, ids, b->result, i);   /* never read */
    }
    snprintf(buf, sizeof(buf), "move.xyzw     VF20,VF%02d", reg_of(&r, ids, sh->positionValue));
    vu(f, "NOP", buf, "gl_Position");
    if (sh->colorValue) {
        snprintf(buf, sizeof(buf), "move.xyzw     VF21,VF%02d", reg_of(&r, ids, sh->colorValue));
        snprintf(buf2, sizeof(buf2), "color output, location %d", sh->colorLocation);
        vu(f, "NOP", buf, buf2);
    } else {
        vu(f, "NOP", "move.xyzw     VF21,VF00", "no color output: black");
    }
    if (sh->texcoordValue) {
        snprintf(buf, sizeof(buf), "move.xy       VF%02d,VF%02d", PVK_VU1_TEXCOORD_REG,
                 reg_of(&r, ids, sh->texcoordValue));
        snprintf(buf2, sizeof(buf2), "texture coordinate output, location %d", sh->texcoordLocation);
        vu(f, "NOP", buf, buf2);
    }

    fprintf(f, "; epilogue: perspective divide, viewport, GS formats, store\n");
    vu(f, "NOP", "div           Q,VF00w,VF20w", "Q = 1 / w");
    vu(f, "NOP", "waitq", NULL);
    vu(f, "mulq.xyz      VF22,VF20,Q", "NOP", NULL);
    if (sh->texcoordValue) {
        /* The GS interpolates S/w, T/w and Q = 1/w and divides per pixel, which
         * makes the texture perspective correct. */
        snprintf(buf, sizeof(buf), "mulq.xy       VF%02d,VF%02d,Q", PVK_VU1_TEXCOORD_REG, PVK_VU1_TEXCOORD_REG);
        vu(f, buf, "NOP", "S / w, T / w");
        snprintf(buf, sizeof(buf), "addq.z        VF%02d,VF00,Q", PVK_VU1_TEXCOORD_REG);
        vu(f, buf, "NOP", "Q = 1 / w");
    }
    vu(f, "mul.xyz       VF22,VF22,VF01", "NOP", "viewport scale");
    vu(f, "add.xyz       VF22,VF22,VF02", "NOP", "viewport offset");
    vu(f, "ftoi4.xy      VF23,VF22", "NOP", "x, y to 12.4 fixed point");
    vu(f, "ftoi0.z       VF23,VF22", "NOP", "z to integer depth");
    vu(f, "mul.xyzw      VF24,VF21,VF03", "NOP", "color 0..1 to GS range");
    vu(f, "ftoi0.xyzw    VF24,VF24", "NOP", NULL);
    if (sh->texcoordValue) {
        snprintf(buf, sizeof(buf), "sq            VF%02d,0(VI05)", PVK_VU1_TEXCOORD_REG);
        vu(f, "NOP", buf, "ST (and Q)");
        vu(f, "NOP", "sq            VF24,1(VI05)", "RGBAQ");
        vu(f, "NOP", "sq            VF23,2(VI05)", "XYZ2 (draws)");
        snprintf(buf, sizeof(buf), "iaddiu        VI05,VI05,%d", PVK_VU1_OUTPUT_PER_VERTEX_TEXTURED);
    } else {
        vu(f, "NOP", "sq            VF24,0(VI05)", "RGBAQ");
        vu(f, "NOP", "sq            VF23,1(VI05)", "XYZ2 (draws)");
        snprintf(buf, sizeof(buf), "iaddiu        VI05,VI05,%d", PVK_VU1_OUTPUT_PER_VERTEX);
    }
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
    fprintf(f, "    .texcoordOutputLocation = %d,\n", sh->texcoordValue ? sh->texcoordLocation : -1);
    fprintf(f, "    .pushConstantBytes = %u,\n", (unsigned)sh->pushBytes);
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

    code = read_spirv(in, &words);
    parse(code, words, ids, &sh);

    if (sh.execModel == ExecVertex)
        emit_vertex(&sh, ids, outBase, sym);
    else
        emit_fragment(&sh, ids, outBase, sym);

    free(code);
    return 0;
}
