/*
 * ps2vk-shaderc: compiles SPIR-V (glslangValidator -V) for the PS2.
 *
 *   .vert --glslangValidator--> .spv --shaderc--> .pvs  (VU1 microcode)
 *   .frag --glslangValidator--> .spv --shaderc--> .pvs  (GS settings)
 *   .comp --glslangValidator--> .spv --shaderc--> .pvs  (VU1 microcode)
 *   .task --glslangValidator--> .spv --shaderc--> .pvs  (VU1 microcode)
 *   .mesh --glslangValidator--> .spv --shaderc--> .pvs  (VU1 microcode)
 *
 * A .pvs file (PvkPvsHeader in ps2vk.h) is what pvkCreateShaderModule() takes.
 * For every stage but fragment shaderc writes VU1 assembly, runs dvp-as on it to get
 * the microcode and deletes the assembly. --listing also writes <output_base>.txt:
 * the .pvs file as text, followed by that assembly.
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
 *             when the count is constant, else VU1 branches), and specialization
 *             constants (float, int, uint, bool and vectors of them), whose
 *             values the pipeline writes into its copy of the code
 *   compute:  what a vertex shader computes with, on storage blocks (std430
 *             buffer blocks, up to PVK_MAX_STORAGE_BLOCKS, each a descriptor
 *             set binding) read and written by member, element or whole
 *             struct, .length() of a runtime array, the components of
 *             gl_GlobalInvocationID, gl_LocalInvocationID, gl_WorkGroupID and
 *             gl_NumWorkGroups, and gl_LocalInvocationIndex
 *   task:     what a compute shader computes with, the task payload and
 *             EmitMeshTasksEXT
 *   mesh:     what a compute shader computes with, the task payload,
 *             SetMeshOutputsEXT, gl_MeshVerticesEXT[].gl_Position, a color,
 *             a texture coordinate and a fog output per vertex, the primitive
 *             index arrays, gl_CullPrimitiveEXT and a perprimitiveEXT color
 *   fragment: outColor = vColor;                      (Gouraud)
 *             outColor = texture(tex, vUV);           (GS TFX decal)
 *             outColor = texture(tex, vUV) * vColor;  (GS TFX modulate)
 *             if (<that color>.a < <constant>) discard;  (GS alpha test)
 *             outColor = vec4(mix(<constant vec3>, <that color>.rgb, vFog), <that color>.a);  (GS fog)
 *
 * Anything else is rejected with an error that says why, the way a Vulkan
 * driver rejects a shader that uses a feature the device lacks.
 *
 * Usage: shaderc [--listing] input.spv -o output_base
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
#define MAX_SPEC_OPS 32
#define MAX_STRUCT_MEMBERS 8

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
    OpConstantComposite = 44, OpSpecConstantTrue = 48, OpSpecConstantFalse = 49, OpSpecConstant = 50,
    OpSpecConstantComposite = 51, OpSpecConstantOp = 52, OpFunction = 54, OpFunctionParameter = 55,
    OpFunctionEnd = 56,
    OpCopyObject = 83,
    OpFunctionCall = 57, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
    OpInBoundsAccessChain = 66, OpArrayLength = 68,
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
    OpControlBarrier = 224, OpMemoryBarrier = 225,
    OpPhi = 245, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249,
    OpBranchConditional = 250, OpSwitch = 251, OpKill = 252, OpReturn = 253, OpReturnValue = 254,
    OpUnreachable = 255, OpNoLine = 317, OpModuleProcessed = 330,
    OpTerminateInvocation = 4416, OpDemoteToHelperInvocation = 5380,
    OpEmitMeshTasksEXT = 5294, OpSetMeshOutputsEXT = 5295,
    OpDynLoad = 0x10000,            /* ours: lq of a block qword through VI08 */
    OpPhiMove = 0x10001,            /* ours: copy a value into a phi's register at the end of a block */
    OpConstLoad = 0x10002,          /* ours: lq of a push constant or uniform qword */
    OpBoolToFloat = 0x10003,        /* ours: a condition as a float, 0 = false, for a bool kept by a phi */
    OpStorLoad = 0x10004,           /* ours: lq of a storage block qword */
    OpStorStore = 0x10005,          /* ours: sq of a value into a storage block qword */
    OpEmitTasks = 0x10006,          /* ours: the mesh work group counts of a task work group, unless it has them */
    OpBarrier = 0x10007             /* ours: barrier(): the invocation waits for the others of its work group */
};
#define LoopControlUnroll     1
#define LoopControlDontUnroll 2

enum { DecSpecId = 1, DecBlock = 2, DecBufferBlock = 3, DecRowMajor = 4, DecColMajor = 5, DecArrayStride = 6, DecMatrixStride = 7,
       DecBuiltIn = 11, DecLocation = 30, DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35,
       DecPerPrimitive = 5271 };
enum { BuiltInPointSize = 1, BuiltInClipDistance = 3, BuiltInCullDistance = 4, BuiltInPrimitiveId = 7,
       BuiltInLayer = 9, BuiltInViewportIndex = 10, BuiltInPrimitiveShadingRate = 4432,
       BuiltInPrimitivePointIndices = 5294, BuiltInPrimitiveLineIndices = 5295,
       BuiltInPrimitiveTriangleIndices = 5296, BuiltInCullPrimitive = 5299 };
enum { BuiltInPosition = 0, BuiltInNumWorkgroups = 24, BuiltInWorkgroupSize = 25, BuiltInWorkgroupId = 26,
       BuiltInLocalInvocationId = 27, BuiltInGlobalInvocationId = 28, BuiltInLocalInvocationIndex = 29,
       BuiltInInstanceIndex = 43 };
enum { ScUniformConstant = 0, ScInput = 1, ScUniform = 2, ScOutput = 3, ScWorkgroup = 4, ScFunction = 7,
       ScPushConstant = 9, ScStorageBuffer = 12, ScTaskPayload = 5402 };
enum { ExecVertex = 0, ExecFragment = 4, ExecGLCompute = 5, ExecTask = 5364, ExecMesh = 5365 };
enum { ExecModeLocalSize = 17, ExecModeOutputVertices = 26, ExecModeOutputPoints = 27, ExecModeLocalSizeId = 38,
       ExecModeOutputLines = 5269, ExecModeOutputPrimitives = 5270, ExecModeOutputTriangles = 5298 };
/* The stages VU1 runs as work groups of invocations. */
#define IS_GROUPED(sh) ((sh)->execModel == ExecGLCompute || (sh)->execModel == ExecTask || (sh)->execModel == ExecMesh)
/* The stages whose code runs on VU1. */
#define IS_CODE(sh) ((sh)->execModel == ExecVertex || IS_GROUPED(sh))

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
    int      specId[4];      /* specialization constant id of each component (SpecId), -1 if fixed */
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
    /* compute: storage blocks and the gl_ built-ins */
    int      bufferBlock;    /* struct type: a storage block (BufferBlock) */
    int      runtimeArray;   /* array type: its length comes from the bound buffer range */
    int      storBlock;      /* pointer: 1 + index into storage[] of the block it points into, 0 if none */
    uint32_t storOff;        /* pointer: byte offset in the block, before the run-time index */
    uint32_t ptrType;        /* pointer: type id of what it points at */
    int      builtinQword;   /* pointer or uvec3 value: 1 + constant qword of a gl_ built-in vector, 0 if none */
    uint32_t smem[MAX_STRUCT_MEMBERS]; /* struct value: the value id of each member */
    int      smemCount;
    /* mesh outputs */
    int      perPrimitive;   /* variable: a perprimitiveEXT output */
    int      ignored;        /* pointer: stores to it change nothing the GS draws (gl_PointSize, gl_PrimitiveID) */
    int      cullFlag;       /* pointer: gl_CullPrimitiveEXT of one primitive */
    int      sharedQword;    /* variable: 1 + its first qword among the shared variables, 0 if not shared */
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
    int      fragAlpha;      /* ALPHA_*: the color this float is the alpha of, or this comparison tests */
    int      fragCmp;        /* CMP_*: a comparison "alpha CMP fragRef" */
    float    fragRef;
    int      fragKind;       /* FK_*: a piece of 'vec4(mix(<constant>, <color>.rgb, <fog>), <color>.a)' */
    int      fragClass;      /* ALPHA_*: the color that piece comes from */
    uint32_t fragSrc;        /* FK_FOGGED: the fog color constant; FK_FOGGED_COMP: the mix */
    int      fragFogged;     /* a color mixed with a fog color */
    const char *name;
} IdInfo;

enum { TEXEL_NONE, TEXEL_SAMPLER, TEXEL_SAMPLE, TEXEL_SAMPLE_TIMES_COLOR };
/* Whose alpha: one of the three colors a fragment shader can write, or whatever outColor holds. */
enum { ALPHA_NONE, ALPHA_COLOR, ALPHA_SAMPLE, ALPHA_SAMPLE_TIMES_COLOR, ALPHA_OUTPUT };
enum { CMP_NONE, CMP_LT, CMP_LE, CMP_GT, CMP_GE, CMP_EQ, CMP_NE };
/* <color>.rgb, vec3(<fog>), the mix of the two with the fog color, one component of that mix */
enum { FK_NONE, FK_RGB, FK_FOG_VEC, FK_FOGGED, FK_FOGGED_COMP };

typedef struct {
    int location;
    int size;                /* 4 (a color) or 2 (a texture coordinate) */
    uint32_t var;
} Var;

/* OpMemberDecorate on struct members (push constant blocks): byte Offset, and
 * for matrix members their MatrixStride and RowMajor/ColMajor layout. */
#define MAX_MEMBER_DECORS 256
typedef struct { uint32_t structType, member, offset, matrixStride; int hasOffset, rowMajor, builtin; } MemberDecor;
static MemberDecor g_memberDecors[MAX_MEMBER_DECORS];
static int g_memberDecorCount;

static const char *g_src = "shader";
static const char *g_stage = NULL;   /* compute, task or mesh: messages name that stage's shader */
static int g_intVectors = 0;         /* mesh and task: an int vector is a vector of the floats VU1 counts with */
static int g_header = -1;            /* first of a mesh or task program's 8 header qwords; -1 = a compute program's */
static int g_synBase[5];             /* VU1 address of the vertex and primitive records, output counts, payload, shared variables */
static int g_ctxQwords = 0;          /* qwords an invocation keeps at a barrier(): where it goes on, and its registers */
static const char *g_sym = "";       /* symbol prefix of the microcode being written */
static int g_clamps = 0;             /* run-time index clamps labelled so far */
#define MAX_SPECS 64
static struct { int id, type; } g_specs[MAX_SPECS]; /* specialization constants labelled so far */
static int g_specCount = 0;
static const char *g_file = NULL;    /* from OpLine/OpString, if present */
static uint32_t g_line = 0;

static void fail(const char *fmt, ...)
{
    char msg[2048];
    const char *s;
    va_list ap;

    if (g_line)
        fprintf(stderr, "%s:%u: error: ", g_file ? g_file : g_src, (unsigned)g_line);
    else
        fprintf(stderr, "%s: error: ", g_src);
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    /* The body compiler is shared: a compute, task or mesh shader's messages name its stage. */
    for (s = msg; *s; s++) {
        int skip = !g_stage ? 0 : !strncmp(s, "vertex shader", 13) ? 6 : !strncmp(s, "compute shader", 14) ? 7 : 0;
        if (skip) {
            fputs(g_stage, stderr);
            s += skip - 1;
        } else {
            fputc(*s, stderr);
        }
    }
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

enum { DISCARD_NONE, DISCARD_IF, DISCARD_ALWAYS };

#define DISCARD_FORM "the GS discards a pixel only by comparing the alpha of the color written " \
                     "with a constant: 'if (<color>.a < <constant>) discard;'"
#define FOG_FORM "the GS fogs a pixel only by mixing its red, green and blue with a constant color by " \
                 "the float input: 'vec4(mix(<constant vec3>, <color>.rgb, <float input>), <color>.a)'"

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

/* Nonzero if the module's entry point is a compute, task or mesh shader. */
static int spirv_is_compute(const char *path)
{
    size_t words, pc;
    uint32_t *code = read_spirv(path, &words);
    int compute = 0;

    for (pc = 5; pc < words && code[pc] >> 16; pc += code[pc] >> 16)
        if ((code[pc] & 0xffff) == OpEntryPoint && pc + 1 < words) {
            compute = code[pc + 1] == ExecGLCompute || code[pc + 1] == ExecTask || code[pc + 1] == ExecMesh;
            break;
        }
    free(code);
    return compute;
}

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
    char cmd[1400];

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
    if (spirv_is_compute(in)) {
        /* A struct copied out of a storage block leaves locals the first rounds keep. */
        snprintf(cmd, sizeof(cmd),
                 "spirv-opt --scalar-replacement=100 --eliminate-local-single-block --eliminate-local-single-store "
                 "--eliminate-local-multi-store --simplify-instructions --eliminate-dead-code-aggressive "
                 "-o \"%s\" \"%s\"", opt, opt);
        if (system(cmd) != 0)
            fail("spirv-opt failed on %s (SPIRV-Tools must be installed)", in);
    }
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
    if (t->type == T_VECTOR && t->elemCount >= 2 && t->elemCount <= 4 &&
        (is_float(ids, t->elemType) || (g_intVectors && is_int(ids, t->elemType))))
        return (int)t->elemCount;
    return 0;
}

static int is_int_vector(const IdInfo *ids, uint32_t type)
{
    return ids[type].type == T_VECTOR && is_int(ids, ids[type].elemType);
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

/* Nonzero if any component of the constant is a specialization constant. */
static int is_spec(const IdInfo *c)
{
    return c->specId[0] >= 0 || c->specId[1] >= 0 || c->specId[2] >= 0 || c->specId[3] >= 0;
}

/* The field a scalar operand will be read from: a constant 1 is VF00's w once parsing is done. */
static int scalar_comp(const IdInfo *ids, uint32_t bound, uint32_t id)
{
    if (id < bound && ids[id].kind == ID_CONST && ids[id].compCount == 1 && !is_spec(&ids[id]))
        return ids[id].comps[0] == 0x3f800000 ? 3 : 0;
    return ids[id].comp;
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
    int      storBlock;             /* OpStorLoad/Store: index into storage[] */
    int      storDyn;               /* OpStorLoad/Store: the address has a run-time index (operand ops[0] or ops[1]) */
    int      storRuntime;           /* OpStorLoad/Store: the index picks an element of the block's runtime array */
    int      storComp, storSize;    /* OpStorStore: first field and number of fields written */
    int      storToInt;             /* OpStorStore: the value is a float, the member an int */
    uint32_t line;                  /* GLSL line, 0 if unknown */
} BodyOp;

enum { OUT_POSITION, OUT_COLOR, OUT_TEXCOORD, OUT_FOG, OUT_COUNT };
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

/* A storage block of a compute shader: a descriptor set binding whose buffer range the
 * dispatch copies into VU1 memory and, when the shader writes it, back. */
typedef struct {
    uint32_t    var;         /* the block variable */
    const char *name;
    int         set, binding;
    uint32_t    bytes;       /* size of the block without its runtime array */
    uint32_t    arrayOffset; /* byte offset of its runtime array */
    uint32_t    arrayStride; /* bytes per element of it, 0 = the block has none */
    int         written;     /* the shader stores to it */
} StorBlock;

/* Blocks at fixed VU1 addresses, numbered after the storage blocks: a mesh shader's vertex and
 * primitive records, its SetMeshOutputsEXT counts, the task payload, and the shared variables. */
enum { SYN_VERT = PVK_MAX_STORAGE_BLOCKS, SYN_PRIM, SYN_COUNTS, SYN_PAYLOAD, SYN_SHARED };

/* VU1 address of header qword k (PVK_VU1_HEADER_*) of the program being compiled. */
static int hdr_addr(int k)
{
    return g_header >= 0 ? g_header + k : k < 4 ? k : PVK_VU1_COMPUTE_WORKGROUP + k - 4;
}

/* Constant qwords a shader may read: the push constants, then the uniform area. */
#define INSTANCE_QWORD (PVK_VU1_PUSH_QWORDS + PVK_VU1_UNIFORM_QWORDS) /* gl_InstanceIndex as a float in z */
/* compute: the dispatch header's and the invocation's qwords */
#define GROUPS_QWORD    (INSTANCE_QWORD + 1) /* gl_NumWorkGroups */
#define WORKGROUP_QWORD (INSTANCE_QWORD + 2) /* gl_WorkGroupID */
#define LOCAL_QWORD     (INSTANCE_QWORD + 3) /* gl_LocalInvocationID, w = gl_LocalInvocationIndex */
#define GLOBAL_QWORD    (INSTANCE_QWORD + 4) /* gl_GlobalInvocationID */
#define LENGTHS_QWORD   (INSTANCE_QWORD + 5) /* .length() of each storage block's runtime array */
#define CONST_QWORDS (INSTANCE_QWORD + 6)

typedef struct {
    uint32_t execModel;
    const char *entryPoint;         /* OpEntryPoint name */
    uint32_t glsl450;               /* id of the GLSL.std.450 import, 0 if none */
    Var      inputs[MAX_INPUTS];
    int      inputCount;
    int      colorInput;            /* fragment: index into inputs[] of the vec4 input, -1 if none */
    int      texcoordInput;         /* fragment: index into inputs[] of the vec2 input, -1 if none */
    uint32_t samplerVar;            /* fragment: the sampler2D variable, 0 if none */
    int      samplerBinding;        /* its binding, -1 if none */
    int      samplerSet;            /* its descriptor set, -1 if none */
    int      textureFunction;       /* fragment: GS TEX0.TFX, 0 modulate, 1 decal */
    int      discard;               /* fragment: DISCARD_* */
    int      discardInv;            /* the discard is on the false side of the 'if' */
    int      branches;              /* fragment: 'if's seen */
    uint32_t branchCond, branchTrue, branchFalse, branchMerge; /* of its one 'if' */
    uint32_t branchLine;            /* source line of that 'if' */
    uint32_t storeLabel;            /* fragment: block of the last store to outColor */
    uint32_t prevColorValue;        /* colorValue and textureFunction before that store */
    int      prevTextureFunction;
    uint32_t colorVar;             /* vertex: the vec4 output variable, 0 if none */
    uint32_t texcoordVar;           /* vertex: the vec2 output variable, 0 if none */
    int      texcoordLocation;      /* -1 if none */
    uint32_t texcoordValue;         /* value id stored to the texcoord output, 0 if never */
    uint32_t fogVar;                /* vertex: the float output variable, 0 if none */
    int      fogLocation;           /* -1 if none */
    uint32_t fogValue;              /* value id stored to the fog output, 0 if never */
    int      fogInput;              /* fragment: index into inputs[] of the float input, -1 if none */
    int      fogged;                /* fragment: the color written is mixed with a fog color */
    uint32_t fogColor;              /* that color as GS FOGCOL */
    int      prevFogged;            /* fogged before the last store to outColor */
    uint32_t pushVar;               /* push constant block variable, 0 if none */
    uint32_t instanceIndexVar;      /* gl_InstanceIndex, 0 if none */
    uint32_t pushBytes;             /* size of the push constant block */
    Block    uniforms[PVK_MAX_UNIFORM_BLOCKS]; /* uniform blocks, sorted by (set, binding) */
    int      uniformCount;
    uint32_t localSize[3];          /* compute: local_size_x, _y, _z */
    StorBlock storage[PVK_MAX_STORAGE_BLOCKS]; /* compute: storage blocks, sorted by (set, binding) */
    int      storageCount;
    uint32_t builtinVars[6];        /* compute: the gl_ built-in input variables, by BuiltIn - 24 */
    uint32_t meshMaxVertices;       /* mesh: max_vertices */
    uint32_t meshMaxPrimitives;     /* mesh: max_primitives */
    int      meshPrimVertices;      /* mesh: vertices per primitive, 1 points, 2 lines, 3 triangles */
    uint32_t meshVertVar;           /* mesh: gl_MeshVerticesEXT, 0 if none */
    uint32_t meshPrimVar;           /* mesh: gl_MeshPrimitivesEXT, 0 if none */
    uint32_t meshIndexVar;          /* mesh: gl_PrimitiveTriangleIndicesEXT or the line or point one, 0 if none */
    int      meshColorPerPrim;      /* mesh: the color output is perprimitiveEXT */
    int      meshCull;              /* mesh: the shader writes gl_CullPrimitiveEXT */
    int      vertQwords, primQwords;/* mesh: qwords of a vertex record and of a primitive record */
    int      colorSlot, texcoordSlot, fogSlot; /* mesh: qword of each output in its record */
    int      kickVertices, kickQwords;         /* mesh: vertices and qwords of a full GIF packet */
    int      vertBase, primBase, kickA, kickB; /* mesh: VU1 addresses of the records and the two GIF packets */
    uint32_t payloadVar;            /* task and mesh: the task payload variable, 0 if none */
    int      payloadQwords;
    int      sharedQwords;          /* task and mesh: qwords of the shared variables */
    int      barriers;              /* task and mesh: barrier() calls */
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
        /* fieldComp is the operand's register field, which already holds its comp. */
        return field_of(ids, sh, m->ops[m->fieldOp[j]], m->fieldComp[j] - ids[m->ops[m->fieldOp[j]]].comp);
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
        if (ids[t].runtimeArray)
            fail("member %d of %s block '%s' is something other than float, "
                 "vec2, vec3, vec4, a matrix, int or an array of vectors or matrices "
                 "(nested blocks are not supported yet)", m, kind, name);
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

/* Bytes a member of a storage block takes (std430), checking it is one VU1 can read:
 * scalars, float vectors, column-major matrices of 16-byte columns, structs and arrays. */
static uint32_t storage_bytes(const IdInfo *ids, uint32_t t, uint32_t structType, uint32_t member, const char *name)
{
    const IdInfo *ti = &ids[t];

    if (is_float(ids, t) || is_int(ids, t))
        return 4;
    if (type_size(ids, t) > 1)
        return 4 * (uint32_t)type_size(ids, t);
    if (matrix_cols(ids, t)) {
        const MemberDecor *d = member_decor(structType, member, 0);
        if (d && d->rowMajor)
            fail("member %u of storage block '%s' is a row_major matrix; VU1 multiplies by columns, so leave "
                 "it column-major (the default)", (unsigned)member, name);
        if (!d || d->matrixStride != 16)
            fail("member %u of storage block '%s' is a matrix whose columns are %u bytes apart; VU1 reads a "
                 "column per 16-byte qword, so use vec4 columns (mat4, mat3x4, mat2x4)", (unsigned)member, name,
                 d ? (unsigned)d->matrixStride : 0u);
        return 16 * (uint32_t)matrix_cols(ids, t);
    }
    if (ti->type == T_STRUCT) {
        uint32_t bytes = 0;
        for (int m = 0; m < ti->memberCount; m++) {
            uint32_t end = member_offset(t, (uint32_t)m) + storage_bytes(ids, ti->members[m], t, (uint32_t)m, name);
            if (end > bytes)
                bytes = end;
        }
        return bytes;
    }
    if (ti->type == T_ARRAY && !ti->runtimeArray) {
        storage_bytes(ids, ti->elemType, structType, member, name);
        return ti->stride * ti->elemCount;
    }
    fail("member %u of storage block '%s' is %s; storage blocks hold float, int, uint, vec2, vec3, vec4, "
         "matrices, structs of them and arrays of all of these", (unsigned)member, name,
         ti->runtimeArray ? "a runtime array that is not its last member" : "of another type (an int vector or a bool)");
    return 0;
}

/* Checks a compute shader's storage block and fills in its sizes. */
static void check_storage(const IdInfo *ids, uint32_t structType, StorBlock *b)
{
    const IdInfo *s = &ids[structType];

    if (s->type != T_STRUCT)
        fail("storage buffer '%s' is not a block; declare it as 'layout(std430, binding = 0) buffer Name { ... } %s;'",
             b->name, b->name);
    for (int m = 0; m < s->memberCount; m++) {
        uint32_t t = s->members[m], off = member_offset(structType, (uint32_t)m), end;
        if (ids[t].runtimeArray && m == s->memberCount - 1) {
            storage_bytes(ids, ids[t].elemType, structType, (uint32_t)m, b->name);
            if (!ids[t].stride)
                fail("corrupt SPIR-V: runtime array of storage block '%s' has no ArrayStride", b->name);
            b->arrayOffset = off;
            b->arrayStride = ids[t].stride;
            end = off;
        } else {
            end = off + storage_bytes(ids, t, structType, (uint32_t)m, b->name);
        }
        if (end > b->bytes)
            b->bytes = end;
    }
}

/* Index into storage[] of the storage block variable, or -1. */
static int storage_index(const Shader *sh, uint32_t var)
{
    for (int i = 0; i < sh->storageCount; i++)
        if (sh->storage[i].var == var)
            return i;
    return -1;
}

/* Sorts the storage blocks by (set, binding), which is the order the dispatch places them in. */
static void place_storage(Shader *sh)
{
    for (int i = 0; i < sh->storageCount; i++)
        for (int j = i + 1; j < sh->storageCount; j++) {
            StorBlock *a = &sh->storage[i], *b = &sh->storage[j];
            if (b->set < a->set || (b->set == a->set && b->binding < a->binding)) {
                StorBlock t = *a; *a = *b; *b = t;
            }
        }
    for (int i = 0; i < sh->storageCount; i++) {
        const StorBlock *b = &sh->storage[i];
        if (i && b->set == sh->storage[i - 1].set && b->binding == sh->storage[i - 1].binding)
            fail("storage blocks '%s' and '%s' are both at set %d, binding %d", sh->storage[i - 1].name, b->name,
                 b->set, b->binding);
        for (int u = 0; u < sh->uniformCount; u++)
            if (sh->uniforms[u].set == b->set && sh->uniforms[u].binding == b->binding)
                fail("storage block '%s' and uniform block '%s' are both at set %d, binding %d", b->name,
                     sh->uniforms[u].name, b->set, b->binding);
    }
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
    /* A task shader's blocks end the area, which leaves its start to the mesh shader's. */
    for (int i = 0; sh->execModel == ExecTask && i < sh->uniformCount; i++)
        sh->uniforms[i].qword += PVK_VU1_UNIFORM_QWORDS - qword;
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
        if (fl[j].src && (ids[fl[j].src].kind != ID_CONST || is_spec(&ids[fl[j].src])))
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
        if (ids[src].kind == ID_CONST && !is_spec(&ids[src])) {
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
    uint32_t *outValue[OUT_COUNT] = { &sh->positionValue, &sh->colorValue, &sh->texcoordValue, &sh->fogValue };
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
/* A specialization constant keeps its default in the code, as LOI immediates
 * the pipeline overwrites. The GS has no code to overwrite. */
static void spec_declared(const IdInfo *ids, const Shader *sh, uint32_t id)
{
    if (is_spec(&ids[id]) && sh->execModel == ExecFragment)
        fail("fragment shader declares specialization constant '%s'; the GS has fixed functions, "
             "so there is nothing for it to change. Specialization constants are only available "
             "in vertex shaders", ids[id].name ? ids[id].name : "constant");
}

static void place_const_loads(IdInfo *ids, Shader *sh)
{
    static BodyOp out[MAX_BODY];
    uint32_t loaded[CONST_QWORDS];   /* in the current block: value id holding qword q, 0 if none */
    uint32_t *outputs[OUT_COUNT] = { &sh->positionValue, &sh->colorValue, &sh->texcoordValue, &sh->fogValue };
    int n = 0;

    memset(loaded, 0, sizeof(loaded));
    for (int i = 0; i <= sh->bodyCount; i++) {
        BodyOp *b = i < sh->bodyCount ? &sh->body[i] : NULL;
        int nops = b ? b->nops : OUT_COUNT;

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

/* Which of the colors a fragment shader can write a vec4 value is, ALPHA_NONE if none. */
static int frag_color(const IdInfo *ids, const Shader *sh, uint32_t id)
{
    if (ids[id].kind != ID_VALUE || ids[id].fragKind)
        return ALPHA_NONE;
    if (ids[id].texel == TEXEL_SAMPLE)
        return ALPHA_SAMPLE;
    if (ids[id].texel == TEXEL_SAMPLE_TIMES_COLOR)
        return ALPHA_SAMPLE_TIMES_COLOR;
    if (ids[id].texel == TEXEL_NONE && sh->colorInput >= 0 && ids[id].fromInput == sh->colorInput &&
        !ids[id].comp && !ids[id].fragAlpha)
        return ALPHA_COLOR;
    return ALPHA_NONE;
}

/* Whose alpha a float value is, ALPHA_NONE if it is no alpha. */
static int frag_alpha(const IdInfo *ids, const Shader *sh, uint32_t id)
{
    if (ids[id].kind != ID_VALUE || ids[id].fragCmp || ids[id].fragKind)
        return ALPHA_NONE;
    if (ids[id].fragAlpha)
        return ids[id].fragAlpha;
    /* vColor.a loaded through an access chain */
    if (sh->colorInput >= 0 && ids[id].fromInput == sh->colorInput && ids[id].comp == 3)
        return ALPHA_COLOR;
    return ALPHA_NONE;
}

static const char *frag_color_name(int whose)
{
    switch (whose) {
    case ALPHA_COLOR: return "the color input";
    case ALPHA_SAMPLE: return "the texture sample";
    case ALPHA_SAMPLE_TIMES_COLOR: return "the texture sample times the color input";
    default: return "another value";
    }
}

/* Nonzero if the value is the fragment shader's float input, the fog amount. */
static int frag_is_fog(const IdInfo *ids, const Shader *sh, uint32_t id)
{
    return ids[id].kind == ID_VALUE && sh->fogInput >= 0 && ids[id].fromInput == sh->fogInput &&
           !ids[id].fragKind && !ids[id].fragAlpha;
}

/* Makes id the color `whose`, fogged: what 'vec4(mix(...), <color>.a)' comes to. */
static void frag_define_fogged(IdInfo *ids, const Shader *sh, uint32_t id, uint32_t type, int whose)
{
    ids[id].kind = ID_VALUE;
    ids[id].valType = type;
    ids[id].texel = whose == ALPHA_SAMPLE ? TEXEL_SAMPLE
                  : whose == ALPHA_SAMPLE_TIMES_COLOR ? TEXEL_SAMPLE_TIMES_COLOR : TEXEL_NONE;
    if (whose == ALPHA_COLOR)
        ids[id].fromInput = sh->colorInput;
    ids[id].fragFogged = 1;
}

/* A constant vec3 in 0..1 as GS FOGCOL: red in bits 0-7, green 8-15, blue 16-23. */
static uint32_t frag_fogcol(const IdInfo *c)
{
    uint32_t out = 0;

    for (int i = 0; i < 3; i++) {
        float v;
        memcpy(&v, &c->comps[i], sizeof(v));
        v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        out |= (uint32_t)(v * 255.0f + 0.5f) << (8 * i);
    }
    return out;
}

/* A float comparison as "alpha CMP constant"; constLeft when the SPIR-V has the constant first. */
static int frag_compare(uint32_t op, int constLeft)
{
    int cmp;

    switch (op) {
    case OpFOrdLessThan: case OpFUnordLessThan:                 cmp = CMP_LT; break;
    case OpFOrdLessThanEqual: case OpFUnordLessThanEqual:       cmp = CMP_LE; break;
    case OpFOrdGreaterThan: case OpFUnordGreaterThan:           cmp = CMP_GT; break;
    case OpFOrdGreaterThanEqual: case OpFUnordGreaterThanEqual: cmp = CMP_GE; break;
    case OpFOrdEqual: case OpFUnordEqual:                       return CMP_EQ;
    case OpFOrdNotEqual: case OpFUnordNotEqual:                 return CMP_NE;
    default:                                                    return CMP_NONE;
    }
    if (!constLeft)
        return cmp;
    return cmp == CMP_LT ? CMP_GT : cmp == CMP_LE ? CMP_GE : cmp == CMP_GT ? CMP_LT : CMP_LE;
}

static int frag_not(int cmp)
{
    switch (cmp) {
    case CMP_LT: return CMP_GE;
    case CMP_LE: return CMP_GT;
    case CMP_GT: return CMP_LE;
    case CMP_GE: return CMP_LT;
    case CMP_EQ: return CMP_NE;
    default:     return CMP_EQ;
    }
}

/* The GS alpha test that passes the pixels the shader keeps. GS alpha is 8 bits with
 * 128 = 1.0, so the comparison is redone on those steps: TEST.ATST and TEST.AREF. */
static void frag_alpha_test(const Shader *sh, const IdInfo *ids, int *atst, int *aref)
{
    enum { NEVER, ALWAYS, LESS, LEQUAL, EQUAL, GEQUAL, GREATER, NOTEQUAL };
    int cmp = ids[sh->branchCond].fragCmp;
    double x = 128.0 * (double)ids[sh->branchCond].fragRef;
    long lo, hi; /* floor and ceiling of x */

    *aref = 0;
    if (sh->discard == DISCARD_ALWAYS) {
        *atst = NEVER;
        return;
    }
    if (sh->discardInv)
        cmp = frag_not(cmp);
    x = x < -1.0 ? -1.0 : x > 256.0 ? 256.0 : x;
    lo = (long)x;
    if ((double)lo > x)
        lo--;
    hi = (double)lo < x ? lo + 1 : lo;

    switch (cmp) {
    case CMP_LT: /* keep alpha >= x */
        *atst = hi <= 0 ? ALWAYS : hi > 255 ? NEVER : GEQUAL;
        *aref = (int)hi;
        break;
    case CMP_LE: /* keep alpha > x */
        *atst = lo < 0 ? ALWAYS : lo >= 255 ? NEVER : GREATER;
        *aref = (int)lo;
        break;
    case CMP_GT: /* keep alpha <= x */
        *atst = lo < 0 ? NEVER : lo >= 255 ? ALWAYS : LEQUAL;
        *aref = (int)lo;
        break;
    case CMP_GE: /* keep alpha < x */
        *atst = hi <= 0 ? NEVER : hi > 255 ? ALWAYS : LESS;
        *aref = (int)hi;
        break;
    case CMP_EQ: /* keep alpha != x */
        *atst = lo == hi && lo >= 0 && lo <= 255 ? NOTEQUAL : ALWAYS;
        *aref = (int)lo;
        break;
    default:     /* keep alpha == x */
        *atst = lo == hi && lo >= 0 && lo <= 255 ? EQUAL : NEVER;
        *aref = (int)lo;
        break;
    }
    if (*atst == NEVER || *atst == ALWAYS)
        *aref = 0;
}

/* An access chain into a storage block, or into an element a chain already picked:
 * a byte offset, and at most one index computed at run time with the element's size. */
static void storage_chain(IdInfo *ids, Shader *sh, const uint32_t *w, uint32_t n)
{
    uint32_t id = w[2], base = w[3], type, off, dynIndex;
    int blk, dynStride, dynCount, runtime;

    if (ids[base].kind == ID_PTR) {
        blk = ids[base].storBlock - 1;
        type = ids[base].ptrType;
        off = ids[base].storOff;
        dynIndex = ids[base].dynIndex;
        dynStride = ids[base].dynStride;
        dynCount = ids[base].dynCount;
        runtime = ids[base].runtimeArray;
    } else {
        blk = storage_index(sh, base);
        type = ids[ids[base].varType].elemType;
        off = 0;
        dynIndex = 0;
        dynStride = dynCount = runtime = 0;
    }
    for (uint32_t i = 4; i < n; i++) {
        uint32_t idx = w[i], v;
        if (ids[idx].kind == ID_CONST && is_spec(&ids[idx]))
            fail("compute shader indexes a storage block with a specialization constant; index with a fixed "
                 "constant, or at run time with an int");
        if (ids[idx].kind != ID_CONST) {
            if (dynIndex)
                fail("compute shader reaches into a storage block with two indices computed at run time; only "
                     "one per access is supported");
            if (ids[idx].kind != ID_VALUE || !(ids[idx].floatIndex || is_int(ids, ids[idx].valType)))
                fail("compute shader indexes a storage block with something other than an int; VU1 addresses "
                     "qwords through VI08");
            if (ids[type].type == T_ARRAY) {
                if (ids[type].stride % 16)
                    fail("compute shader indexes an array of %u-byte elements in storage block '%s' at run time; "
                         "VU1 addresses qwords, so make the elements whole 16-byte qwords (vec4, mat4, a struct "
                         "of 16, 32, ... bytes)", (unsigned)ids[type].stride, sh->storage[blk].name);
                dynStride = (int)(ids[type].stride / 16);
                dynCount = (int)ids[type].elemCount;
                runtime = ids[type].runtimeArray;
            } else if (ids[type].type == T_MATRIX) {
                dynStride = 1;
                dynCount = (int)ids[type].elemCount;
                runtime = 0;
            } else if (ids[type].type == T_VECTOR) {
                fail("compute shader picks a vector's component in storage block '%s' at run time; VU1 "
                     "addresses qwords, so index an array of vec4 instead", sh->storage[blk].name);
            } else {
                fail("corrupt SPIR-V: storage access chain indexes a scalar or a struct at run time");
            }
            dynIndex = idx;
            type = ids[type].elemType;
            continue;
        }
        v = ids[idx].value;
        if (ids[type].type == T_STRUCT) {
            if (v >= (uint32_t)ids[type].memberCount)
                fail("corrupt SPIR-V: storage block member index out of range");
            off += member_offset(type, v);
            type = ids[type].members[v];
        } else if (ids[type].type == T_ARRAY) {
            if (!ids[type].runtimeArray && v >= ids[type].elemCount)
                fail("compute shader reaches element %u of an array of %u in storage block '%s'", (unsigned)v,
                     (unsigned)ids[type].elemCount, sh->storage[blk].name);
            off += ids[type].stride * v;
            type = ids[type].elemType;
        } else if (ids[type].type == T_MATRIX) {
            off += 16 * v;
            type = ids[type].elemType;
        } else if (ids[type].type == T_VECTOR) {
            off += 4 * v;
            type = ids[type].elemType;
        } else {
            fail("corrupt SPIR-V: storage access chain indexes a scalar");
        }
    }
    ids[id].kind = ID_PTR;
    ids[id].storBlock = blk + 1;
    ids[id].storOff = off;
    ids[id].ptrType = type;
    ids[id].comp = (int)(off % 16 / 4);
    ids[id].dynIndex = dynIndex;
    ids[id].dynStride = dynStride;
    ids[id].dynCount = dynCount;
    ids[id].runtimeArray = runtime;
}

/* Fills in a storage load or store's address: the block, the qword of `off` and the run-time
 * index (as the float VU1 counts with) in ops[at]. The index conversion goes first in the body,
 * so the caller computes `src` before it adds the op. */
static void storage_address(const IdInfo *ids, const Shader *sh, uint32_t ptr, uint32_t off, BodyOp *b,
                            uint32_t src, int at)
{
    float last = (float)(ids[ptr].dynCount - 1);

    (void)sh;
    b->storBlock = ids[ptr].storBlock - 1;
    b->dynBase = (int)(off / 16) + (b->storBlock >= SYN_VERT ? g_synBase[b->storBlock - SYN_VERT] : 0);
    if (!ids[ptr].dynIndex)
        return;
    b->storDyn = 1;
    b->storRuntime = ids[ptr].runtimeArray;
    b->dynStride = ids[ptr].dynStride;
    memcpy(&b->dynLast, &last, sizeof(last));
    b->ops[at] = src;
    b->nops = at + 1;
    b->dynComp = ids[src].comp;
}

static void storage_load(IdInfo *ids, Shader *sh, uint32_t id, uint32_t type, uint32_t ptr, uint32_t extra)
{
    uint32_t off = ids[ptr].storOff + extra, src = 0, q = id;
    int size = is_int(ids, type) ? 1 : type_size(ids, type), comp = (int)(off % 16 / 4);
    BodyOp *b;

    if (!size)
        fail("compute shader reads something other than a float, int, uint, vector, matrix or a struct of "
             "them from a storage block");
    if (is_int_vector(ids, type))
        fail("compute shader reads an int vector back from VU1 memory; read its components one at a time");
    if (comp + size > 4)
        fail("storage block member at byte %u crosses a 16-byte boundary, which VU1 cannot read as one "
             "value", (unsigned)off);
    if (ids[ptr].dynIndex)
        src = int_operand(ids, sh, ids[ptr].dynIndex, sh->body, &sh->bodyCount);
    if (size > 1 && comp)
        q = new_id(sh);
    ids[q].kind = ID_VALUE;
    ids[q].valType = type;
    b = new_op(sh->body, &sh->bodyCount, OpStorLoad, q);
    storage_address(ids, sh, ptr, off, b, src, 0);
    if (size == 1) {
        ids[id].comp = comp;
    } else if (q != id) {
        /* A vector past field x: copied to the leading fields like a constant one. */
        Field fl[4];
        for (int j = 0; j < size; j++) {
            fl[j].src = q;
            fl[j].comp = comp + j;
        }
        define_map(ids, sh, OpLoad, id, type, fl, size);
    }
}

static void storage_store(IdInfo *ids, Shader *sh, uint32_t ptr, uint32_t type, uint32_t extra, uint32_t val,
                          const char *stage)
{
    uint32_t off = ids[ptr].storOff + extra, src = 0;
    int size, toInt = 0;
    BodyOp *b;

    if (ids[type].type == T_STRUCT && ids[ptr].storBlock > SYN_VERT)
        fail("compute shader stores a whole struct to a mesh output or the task payload; write its members one "
             "at a time");
    if (ids[type].type == T_STRUCT) {
        if (ids[val].smemCount != ids[type].memberCount)
            fail("compute shader stores a struct to a storage block that is not built from its members");
        for (int m = 0; m < ids[type].memberCount; m++)
            storage_store(ids, sh, ptr, ids[type].members[m], extra + member_offset(type, (uint32_t)m),
                          ids[val].smem[m], stage);
        return;
    }
    if (matrix_cols(ids, type))
        fail("compute shader writes a whole matrix to a storage block; write its columns one at a time");
    size = is_int(ids, type) ? 1 : type_size(ids, type);
    if (!size)
        fail("compute shader writes something other than a float, int, uint, vector or a struct of them to a "
             "storage block");
    if (is_int(ids, type) && !(ids[val].kind == ID_VALUE && is_int(ids, ids[val].valType) && !ids[val].intIsFloat &&
                               !ids[val].floatIndex && !ids[val].instanceIndex)) {
        /* VU1 counted with it as a float: back to the integer the member holds. */
        val = int_operand(ids, sh, val, sh->body, &sh->bodyCount);
        toInt = 1;
    }
    if (is_int_vector(ids, type))
        toInt = 1;   /* built from the floats VU1 counts with */
    if (value_size(ids, val, stage) != size)
        fail("corrupt SPIR-V: a store to a storage block of a value of another size");
    if ((int)(off % 16 / 4) + size > 4)
        fail("storage block member at byte %u crosses a 16-byte boundary, which VU1 cannot write as one "
             "value", (unsigned)off);
    if (ids[ptr].dynIndex)
        src = int_operand(ids, sh, ids[ptr].dynIndex, sh->body, &sh->bodyCount);
    b = new_op(sh->body, &sh->bodyCount, OpStorStore, 0);
    b->ops[b->nops++] = val;
    storage_address(ids, sh, ptr, off, b, src, 1);
    b->storComp = (int)(off % 16 / 4);
    b->storSize = size;
    b->storToInt = toInt;
    if (b->storBlock < SYN_VERT)
        sh->storage[b->storBlock].written = 1;
}

/* Qwords a mesh output or task payload type takes: one per scalar, vector or matrix column. */
static int plain_qwords(const IdInfo *ids, uint32_t t, const char *what)
{
    const IdInfo *ti = &ids[t];
    int n = 0;

    if (is_float(ids, t) || is_int(ids, t) || type_size(ids, t))
        return 1;
    if (matrix_cols(ids, t))
        return matrix_cols(ids, t);
    if (ti->type == T_ARRAY && !ti->runtimeArray)
        return (int)ti->elemCount * plain_qwords(ids, ti->elemType, what);
    if (ti->type != T_STRUCT)
        fail("%s holds something other than floats, ints, vectors, matrices and structs and arrays of them", what);
    for (int m = 0; m < ti->memberCount; m++)
        n += plain_qwords(ids, ti->members[m], what);
    return n;
}

/* Nonzero if an access chain from `base` reaches into a mesh output or the task payload. */
static int is_plain_base(const IdInfo *ids, const Shader *sh, uint32_t base)
{
    if (ids[base].kind == ID_PTR)
        return ids[base].ignored || ids[base].storBlock > SYN_VERT;
    if (!base || sh->execModel == ExecVertex || sh->execModel == ExecFragment)
        return 0;
    return base == sh->payloadVar || ids[base].sharedQword || (sh->execModel == ExecMesh &&
           (base == sh->meshVertVar || base == sh->meshPrimVar || base == sh->meshIndexVar ||
            base == sh->colorVar || base == sh->texcoordVar || base == sh->fogVar));
}

/* An access chain into a mesh output array or the task payload: a qword at a fixed address, a
 * field of it, and at most one index computed at run time. */
static void plain_chain(IdInfo *ids, Shader *sh, const uint32_t *w, uint32_t n)
{
    uint32_t id = w[2], base = w[3], type, dynIndex = 0, i = 4;
    int blk, q = 0, comp = 0, dynStride = 0, dynCount = 0, cull = 0;
    const char *what = "a mesh output";

    if (ids[base].kind == ID_PTR && ids[base].ignored) {
        ids[id].kind = ID_PTR;
        ids[id].ignored = 1;
        return;
    }
    if (ids[base].kind == ID_PTR) {
        blk = ids[base].storBlock - 1;
        type = ids[base].ptrType;
        q = (int)(ids[base].storOff / 16);
        comp = (int)(ids[base].storOff % 16 / 4);
        dynIndex = ids[base].dynIndex;
        dynStride = ids[base].dynStride;
        dynCount = ids[base].dynCount;
    } else if (base == sh->payloadVar) {
        blk = SYN_PAYLOAD;
        type = ids[ids[base].varType].elemType;
    } else if (ids[base].sharedQword) {
        blk = SYN_SHARED;
        type = ids[ids[base].varType].elemType;
        q = ids[base].sharedQword - 1;
    } else {
        /* An output array: the first index picks the vertex or the primitive. */
        uint32_t arr = ids[ids[base].varType].elemType, idx;
        int perVertex = base == sh->meshVertVar || base == sh->texcoordVar || base == sh->fogVar ||
                        (base == sh->colorVar && !sh->meshColorPerPrim);
        int stride = perVertex ? sh->vertQwords : sh->primQwords;
        int slot = base == sh->colorVar ? sh->colorSlot : base == sh->texcoordVar ? sh->texcoordSlot
                 : base == sh->fogVar ? sh->fogSlot : 0;

        if (n < 5)
            fail("mesh shader reads or writes a whole output array; use its elements one at a time");
        blk = perVertex ? SYN_VERT : SYN_PRIM;
        idx = w[4];
        i = 5;
        if (ids[idx].kind == ID_CONST && !is_spec(&ids[idx])) {
            if (ids[idx].value >= ids[arr].elemCount)
                fail("mesh shader reaches element %u of an output array of %u", (unsigned)ids[idx].value,
                     (unsigned)ids[arr].elemCount);
            q = (int)ids[idx].value * stride + slot;
        } else {
            if (ids[idx].kind != ID_VALUE || !(ids[idx].floatIndex || is_int(ids, ids[idx].valType)))
                fail("mesh shader indexes an output array with something other than an int; VU1 addresses "
                     "qwords through VI08");
            dynIndex = idx;
            dynStride = stride;
            dynCount = (int)ids[arr].elemCount;
            q = slot;
        }
        type = ids[arr].elemType;
        if (base == sh->meshVertVar || base == sh->meshPrimVar) {
            /* A member of gl_MeshPerVertexEXT or gl_MeshPerPrimitiveEXT, known by its BuiltIn. */
            const MemberDecor *d;
            if (n < 6 || ids[w[5]].kind != ID_CONST || ids[w[5]].value >= (uint32_t)ids[type].memberCount)
                fail("mesh shader reads or writes a whole gl_MeshVerticesEXT or gl_MeshPrimitivesEXT element; "
                     "use its members one at a time");
            d = member_decor(type, ids[w[5]].value, 0);
            type = ids[type].members[ids[w[5]].value];
            i = 6;
            switch (d ? d->builtin - 1 : -1) {
            case BuiltInPosition:
                break;
            case BuiltInPointSize: case BuiltInPrimitiveId:
                /* A GS point is one pixel, and no fragment shader reads gl_PrimitiveID. */
                ids[id].kind = ID_PTR;
                ids[id].ignored = 1;
                return;
            case BuiltInCullPrimitive:
                comp = 3;
                cull = 1;
                sh->meshCull = 1;
                break;
            case BuiltInClipDistance: case BuiltInCullDistance:
                fail("mesh shader uses gl_ClipDistance or gl_CullDistance; the GS clips to the scissor "
                     "rectangle only (shaderClipDistance, shaderCullDistance)");
                break;
            case BuiltInLayer: case BuiltInViewportIndex:
                fail("mesh shader uses gl_Layer or gl_ViewportIndex; the GS draws into one image through one "
                     "viewport (maxMeshOutputLayers, multiViewport)");
                break;
            default:
                fail("mesh shader uses a member of gl_MeshVerticesEXT or gl_MeshPrimitivesEXT other than "
                     "gl_Position, gl_PointSize, gl_PrimitiveID and gl_CullPrimitiveEXT");
            }
        }
    }
    if (blk == SYN_PAYLOAD)
        what = "the task payload";
    else if (blk == SYN_SHARED)
        what = "a shared variable";
    for (; i < n; i++) {
        uint32_t idx = w[i], v;
        if (ids[idx].kind != ID_CONST || is_spec(&ids[idx])) {
            if (dynIndex)
                fail("compute shader reaches into %s with two indices computed at run time; only one per "
                     "access is supported", what);
            if (ids[idx].kind != ID_VALUE || !(ids[idx].floatIndex || is_int(ids, ids[idx].valType)))
                fail("compute shader indexes %s with something other than an int; VU1 addresses qwords "
                     "through VI08", what);
            if (ids[type].type == T_ARRAY)
                dynStride = plain_qwords(ids, ids[type].elemType, what);
            else if (ids[type].type == T_MATRIX)
                dynStride = 1;
            else
                fail("compute shader picks a vector's component in %s at run time; VU1 addresses qwords, so "
                     "index an array instead", what);
            dynIndex = idx;
            dynCount = (int)ids[type].elemCount;
            type = ids[type].elemType;
            continue;
        }
        v = ids[idx].value;
        if (ids[type].type == T_STRUCT && v < (uint32_t)ids[type].memberCount) {
            for (uint32_t m = 0; m < v; m++)
                q += plain_qwords(ids, ids[type].members[m], what);
            type = ids[type].members[v];
        } else if ((ids[type].type == T_ARRAY || ids[type].type == T_MATRIX) && v < ids[type].elemCount) {
            q += (int)v * (ids[type].type == T_ARRAY ? plain_qwords(ids, ids[type].elemType, what) : 1);
            type = ids[type].elemType;
        } else if (ids[type].type == T_VECTOR && v < ids[type].elemCount) {
            comp += (int)v;
            type = ids[type].elemType;
        } else {
            fail("compute shader reaches a member, element or component of %s that does not exist", what);
        }
    }
    ids[id].kind = ID_PTR;
    ids[id].storBlock = blk + 1;
    ids[id].storOff = (uint32_t)q * 16 + (uint32_t)comp * 4;
    ids[id].ptrType = type;
    ids[id].comp = comp;
    ids[id].dynIndex = dynIndex;
    ids[id].dynStride = dynStride;
    ids[id].dynCount = dynCount;
    ids[id].cullFlag = cull;
}

/* gl_CullPrimitiveEXT = <bool>: nonzero in the w field of the primitive's record. */
/* A pointer to a whole shared variable, for a load or a store that names the variable itself. */
static uint32_t shared_ptr(IdInfo *ids, Shader *sh, uint32_t var)
{
    uint32_t p = new_id(sh);

    ids[p].kind = ID_PTR;
    ids[p].storBlock = SYN_SHARED + 1;
    ids[p].storOff = (uint32_t)(ids[var].sharedQword - 1) * 16;
    ids[p].ptrType = ids[ids[var].varType].elemType;
    return p;
}

static void cull_store(IdInfo *ids, Shader *sh, uint32_t ptr, uint32_t val)
{
    uint32_t fl = new_id(sh), src = 0;
    BodyOp *b;

    ids[fl].valType = sh->floatType;
    if (ids[val].kind == ID_CONST && !ids[val].boolKind) {
        float v = ids[val].value ? 1.0f : 0.0f;
        ids[fl].kind = ID_CONST;
        memcpy(&ids[fl].comps[0], &v, sizeof(v));
        ids[fl].compCount = 1;
    } else {
        ids[fl].kind = ID_VALUE;
        if (!ids[val].boolKind)
            fail("mesh shader stores something other than a comparison or a constant to gl_CullPrimitiveEXT");
        if (ids[val].boolKind == BOOL_FLOAT) {
            fl = ids[val].boolA;
        } else {
            b = new_op(sh->body, &sh->bodyCount, OpBoolToFloat, fl);
            b->cond = val;
            collect_leaves(ids, val, b);
        }
    }
    if (ids[ptr].dynIndex)
        src = int_operand(ids, sh, ids[ptr].dynIndex, sh->body, &sh->bodyCount);
    b = new_op(sh->body, &sh->bodyCount, OpStorStore, 0);
    b->ops[b->nops++] = fl;
    storage_address(ids, sh, ptr, ids[ptr].storOff, b, src, 1);
    b->storComp = 3;
    b->storSize = 1;
    b->storToInt = 1;
}

static void parse(const uint32_t *code, size_t words, IdInfo *ids, Shader *sh)
{
    size_t pc;
    uint32_t bound = code[3];
    int inFunction = 0, functions = 0, specOpCount = 0, replayed = 0, labels = 0;
    const uint32_t *specOps[MAX_SPEC_OPS];

    /* Ids past the SPIR-V bound stand for the push constant and uniform qwords. */
    if (bound + CONST_QWORDS > MAX_IDS)
        fail("shader is too large (%u ids)", (unsigned)bound);

    memset(sh, 0, sizeof(*sh));
    sh->execModel = ~0u;
    sh->colorLocation = sh->texcoordLocation = sh->fogLocation = -1;
    sh->colorInput = sh->texcoordInput = sh->fogInput = sh->samplerBinding = sh->samplerSet = -1;
    sh->constBase = bound;
    sh->nextId = bound + CONST_QWORDS;
    for (uint32_t i = 0; i < MAX_IDS; i++) {
        ids[i].location = ids[i].binding = ids[i].set = ids[i].builtin = ids[i].structPosMember = -1;
        ids[i].fromInput = ids[i].fromConst = -1;
        ids[i].specId[0] = ids[i].specId[1] = ids[i].specId[2] = ids[i].specId[3] = -1;
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
            if (w[1] != 1 && w[1] != 5379 && w[1] != 5283) /* Shader, DemoteToHelperInvocation, MeshShadingEXT */
                fail("shader needs SPIR-V capability %u, which the PS2 does not support", (unsigned)w[1]);
            break;
        case OpEntryPoint:
            if (sh->execModel != ~0u)
                fail("only one entry point per file is supported");
            sh->execModel = w[1];
            sh->entryPoint = (const char *)&w[3];
            g_intVectors = w[1] == ExecTask || w[1] == ExecMesh;
            g_stage = w[1] == ExecGLCompute ? "compute" : w[1] == ExecTask ? "task" : w[1] == ExecMesh ? "mesh" : NULL;
            break;
        case OpString:
            ids[w[1]].name = (const char *)&w[2];
            break;
        case OpExecutionMode:
            if (w[2] == ExecModeLocalSize && n >= 6) {
                sh->localSize[0] = w[3];
                sh->localSize[1] = w[4];
                sh->localSize[2] = w[5];
            } else if (w[2] == ExecModeLocalSizeId) {
                fail("compute shader sets its local size with specialization constants (local_size_x_id); the "
                     "microcode's loops are written for the size, so give it fixed numbers");
            } else if (w[2] == ExecModeOutputVertices && n >= 4) {
                sh->meshMaxVertices = w[3];
            } else if (w[2] == ExecModeOutputPrimitives && n >= 4) {
                sh->meshMaxPrimitives = w[3];
            } else if (w[2] == ExecModeOutputPoints || w[2] == ExecModeOutputLines || w[2] == ExecModeOutputTriangles) {
                sh->meshPrimVertices = w[2] == ExecModeOutputPoints ? 1 : w[2] == ExecModeOutputLines ? 2 : 3;
            }
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
            else if (w[2] == DecSpecId)
                ids[w[1]].specId[0] = (int)w[3];
            else if (w[2] == DecBufferBlock)
                ids[w[1]].bufferBlock = 1;
            else if (w[2] == DecPerPrimitive)
                ids[w[1]].perPrimitive = 1;
            break;
        case OpMemberDecorate:
            if (w[3] == DecBuiltIn) {
                member_decor(w[1], w[2], 1)->builtin = (int)w[4] + 1;
                if (w[4] == BuiltInPosition)
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
            if (ids[w[3]].kind == ID_CONST && is_spec(&ids[w[3]]))
                fail("an array's length is a specialization constant; VU1 memory is laid out when "
                     "the shader is compiled, so give it a fixed length");
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
        case OpTypeRuntimeArray:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_ARRAY;
            ids[w[1]].elemType = w[2];
            ids[w[1]].runtimeArray = 1;
            break;
        case OpTypeImage: case OpTypeSampler:
        case OpTypeVoid:
        case OpTypeFunction:
            ids[w[1]].kind = ID_TYPE; ids[w[1]].type = T_OTHER;
            break;
        case OpConstant: case OpSpecConstant:
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
            spec_declared(ids, sh, w[2]);
            break;
        case OpConstantTrue: case OpConstantFalse:
            ids[w[2]].kind = ID_CONST; ids[w[2]].value = op == OpConstantTrue;
            ids[w[2]].valType = w[1];
            break;
        case OpSpecConstantTrue: case OpSpecConstantFalse: {
            /* A bool VU1 tests as a float: 1.0 or 0.0 until the pipeline sets it. */
            float v = op == OpSpecConstantTrue ? 1.0f : 0.0f;
            ids[w[2]].kind = ID_CONST; ids[w[2]].value = op == OpSpecConstantTrue;
            ids[w[2]].valType = w[1];
            memcpy(&ids[w[2]].comps[0], &v, sizeof(v));
            ids[w[2]].compCount = 1;
            ids[w[2]].boolKind = BOOL_FLOAT;
            ids[w[2]].boolA = w[2];
            spec_declared(ids, sh, w[2]);
            break;
        }
        case OpConstantComposite: case OpSpecConstantComposite: {
            uint32_t id = w[2];
            ids[id].kind = ID_CONST;
            ids[id].valType = w[1];
            if (type_size(ids, w[1]) > 1 && n - 3 <= 4) {
                for (uint32_t i = 0; i < n - 3; i++) {
                    ids[id].comps[i] = ids[w[3 + i]].comps[0];
                    ids[id].specId[i] = ids[w[3 + i]].specId[0];
                }
                ids[id].compCount = (int)(n - 3);
                spec_declared(ids, sh, id);
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
            } else if (!inFunction && storage == ScWorkgroup && sh->execModel == ExecGLCompute) {
                fail("compute shader declares shared variable '%s'; VU1 runs a work group's invocations one "
                     "after another, so there is no shared memory to share", ids[id].name ? ids[id].name : "variable");
            } else if (!inFunction && storage != ScInput && storage != ScOutput && storage != ScPushConstant
                       && storage != ScUniform && !(storage == ScStorageBuffer && IS_GROUPED(sh))
                       && !(storage == ScTaskPayload && IS_GROUPED(sh))
                       && !(storage == ScWorkgroup && IS_GROUPED(sh))) {
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

    if (!IS_CODE(sh) && sh->execModel != ExecFragment)
        fail("only vertex, fragment, compute, task and mesh shaders are supported");
    g_header = sh->execModel == ExecMesh ? PVK_VU1_MESH_ADDR : sh->execModel == ExecTask ? PVK_VU1_TASK_ADDR : -1;
    for (int i = 0; IS_GROUPED(sh) && i < 3; i++)
        if (sh->localSize[i] < 1 || sh->localSize[i] > PVK_MAX_COMPUTE_WORK_GROUP_SIZE)
            fail("compute shader's local size along %c is %u; it must be 1 to %d (%s)",
                 "xyz"[i], (unsigned)sh->localSize[i], PVK_MAX_COMPUTE_WORK_GROUP_SIZE,
                 sh->execModel == ExecMesh ? "maxMeshWorkGroupSize" : sh->execModel == ExecTask
                 ? "maxTaskWorkGroupSize" : "maxComputeWorkGroupSize");
    if (sh->execModel == ExecMesh) {
        if (!sh->meshPrimVertices)
            fail("mesh shader declares no output primitive; write 'layout(triangles, max_vertices = n, "
                 "max_primitives = m) out;'");
        if (sh->meshMaxVertices < 1 || sh->meshMaxVertices > PVK_MAX_MESH_OUTPUT_VERTICES)
            fail("mesh shader's max_vertices is %u; it must be 1 to %d (maxMeshOutputVertices)",
                 (unsigned)sh->meshMaxVertices, PVK_MAX_MESH_OUTPUT_VERTICES);
        if (sh->meshMaxPrimitives < 1 || sh->meshMaxPrimitives > PVK_MAX_MESH_OUTPUT_PRIMITIVES)
            fail("mesh shader's max_primitives is %u; it must be 1 to %d (maxMeshOutputPrimitives)",
                 (unsigned)sh->meshMaxPrimitives, PVK_MAX_MESH_OUTPUT_PRIMITIVES);
    }
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

            if (IS_GROUPED(sh) && storage == ScInput) {
                int b = ids[id].builtin;
                if (b < BuiltInNumWorkgroups || b > BuiltInLocalInvocationIndex || b == BuiltInWorkgroupSize)
                    fail("compute shader input '%s' is none of gl_GlobalInvocationID, gl_LocalInvocationID, "
                         "gl_WorkGroupID, gl_NumWorkGroups and gl_LocalInvocationIndex; a compute shader has no "
                         "other inputs", name);
                sh->builtinVars[b - BuiltInNumWorkgroups] = id;
                ids[id].builtinQword = 1 + (b == BuiltInNumWorkgroups ? GROUPS_QWORD
                                          : b == BuiltInWorkgroupId ? WORKGROUP_QWORD
                                          : b == BuiltInGlobalInvocationId ? GLOBAL_QWORD : LOCAL_QWORD);
            } else if ((sh->execModel == ExecGLCompute || sh->execModel == ExecTask) && storage == ScOutput) {
                fail("compute shader declares output '%s'; a compute shader writes its results to storage "
                     "buffers", name);
            } else if (storage == ScTaskPayload) {
                if (sh->payloadVar)
                    fail("shader declares two task payloads; only one is allowed");
                sh->payloadVar = id;
                sh->payloadQwords = plain_qwords(ids, pointee, "the task payload");
                if (sh->payloadQwords * 16 > PVK_MAX_TASK_PAYLOAD_BYTES)
                    fail("task payload '%s' takes %d bytes of VU1 memory, a 16-byte qword per scalar, vector or "
                         "matrix column; at most %d (maxTaskPayloadSize)", name, sh->payloadQwords * 16,
                         PVK_MAX_TASK_PAYLOAD_BYTES);
            } else if (storage == ScWorkgroup) {
                ids[id].sharedQword = 1 + sh->sharedQwords;
                sh->sharedQwords += plain_qwords(ids, pointee, "a shared variable");
                if (sh->sharedQwords * 16 > PVK_MAX_SHARED_BYTES)
                    fail("shared variables take %d bytes of VU1 memory with '%s', a 16-byte qword per scalar, "
                         "vector or matrix column; at most %d (%s)", sh->sharedQwords * 16, name,
                         PVK_MAX_SHARED_BYTES,
                         sh->execModel == ExecMesh ? "maxMeshSharedMemorySize" : "maxTaskSharedMemorySize");
            } else if (storage == ScOutput && sh->execModel == ExecMesh) {
                uint32_t elem = ids[pointee].elemType;
                int b = ids[id].builtin, size;

                if (ids[pointee].type != T_ARRAY)
                    fail("mesh output '%s' is not an array with an element per vertex or per primitive", name);
                size = type_size(ids, elem);
                if (b == BuiltInPrimitiveTriangleIndices || b == BuiltInPrimitiveLineIndices ||
                    b == BuiltInPrimitivePointIndices) {
                    sh->meshIndexVar = id;
                } else if (ids[elem].type == T_STRUCT && ids[elem].structPosMember >= 0) {
                    sh->meshVertVar = id;
                } else if (ids[elem].type == T_STRUCT) {
                    sh->meshPrimVar = id;
                } else if (b >= 0) {
                    fail("built-in output '%s' is not supported", name);
                } else if (ids[id].location < 0) {
                    fail("output '%s' has no location", name);
                } else if (size == 4 && !is_int_vector(ids, elem)) {
                    if (sh->colorVar)
                        fail("shader has 2 color outputs; the GS can interpolate only one color");
                    sh->colorVar = id;
                    sh->colorLocation = ids[id].location;
                    sh->meshColorPerPrim = ids[id].perPrimitive;
                } else if (ids[id].perPrimitive) {
                    fail("per-primitive mesh output '%s' must be a vec4 (the color); the GS takes nothing else "
                         "per primitive", name);
                } else if (size == 2 && !is_int_vector(ids, elem)) {
                    if (sh->texcoordVar)
                        fail("mesh shader has 2 texture coordinate outputs; the GS interpolates only one pair "
                             "(S, T)");
                    sh->texcoordVar = id;
                    sh->texcoordLocation = ids[id].location;
                } else if (is_float(ids, elem)) {
                    if (sh->fogVar)
                        fail("mesh shader has 2 float outputs; the GS interpolates only one fog amount");
                    sh->fogVar = id;
                    sh->fogLocation = ids[id].location;
                } else {
                    fail("mesh output '%s' must be a vec4 (the color), a vec2 (the texture coordinate) or a float "
                         "(the fog amount); the GS interpolates nothing else", name);
                }
            } else if (storage == ScStorageBuffer || (storage == ScUniform && ids[pointee].bufferBlock)) {
                StorBlock *b;

                if (!IS_GROUPED(sh))
                    fail("'%s' is a storage buffer; storage buffers are only available in compute, task and mesh "
                         "shaders", name);
                if (sh->storageCount == PVK_MAX_STORAGE_BLOCKS)
                    fail("compute shader has more than %d storage blocks (maxPerStageDescriptorStorageBuffers)",
                         PVK_MAX_STORAGE_BLOCKS);
                if (ids[id].binding < 0)
                    fail("storage block '%s' has no binding; give it layout(set = s, binding = b)", name);
                b = &sh->storage[sh->storageCount++];
                memset(b, 0, sizeof(*b));
                b->var = id;
                b->name = name;
                b->set = ids[id].set < 0 ? 0 : ids[id].set; /* no layout(set) means set 0 */
                b->binding = ids[id].binding;
                check_storage(ids, pointee, b);
            } else if (storage == ScInput && ids[id].builtin == BuiltInInstanceIndex && sh->execModel == ExecVertex) {
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
                } else if (size == 1 && is_float(ids, pointee)) {
                    if (sh->fogInput >= 0)
                        fail("fragment shader has two float inputs; the GS interpolates only one "
                             "fog amount");
                    sh->fogInput = sh->inputCount;
                } else {
                    fail("fragment input '%s' must be a vec4 (the color), a vec2 (the texture "
                         "coordinate) or a float (the fog amount); the GS interpolates nothing else", name);
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
                    } else if (size == 1 && is_float(ids, pointee) && sh->execModel == ExecVertex) {
                        if (sh->fogVar)
                            fail("vertex shader has 2 float outputs; the GS interpolates only one "
                                 "fog amount");
                        sh->fogVar = id;
                        sh->fogLocation = ids[id].location;
                    } else if (sh->execModel == ExecVertex) {
                        fail("vertex output '%s' must be a vec4 (the color), a vec2 (the texture "
                             "coordinate) or a float (the fog amount); the GS interpolates nothing else", name);
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
            else if (sh->inputs[i].size == 2)
                sh->texcoordInput = i;
            else
                sh->fogInput = i;
        }
    }

    if (sh->execModel == ExecFragment) {
        if (!sh->colorVar || sh->colorLocation != 0)
            fail("fragment shader must have exactly one vec4 output at location 0");
        if (sh->samplerVar && sh->texcoordInput < 0)
            fail("fragment shader declares a sampler but has no vec2 input to sample it with");
    }
    place_uniforms(sh);
    if (IS_GROUPED(sh))
        place_storage(sh);
    g_synBase[SYN_PAYLOAD - SYN_VERT] = PVK_VU1_PAYLOAD_ADDR;
    g_synBase[SYN_SHARED - SYN_VERT] = PVK_VU1_PAYLOAD_ADDR + sh->payloadQwords;
    if (sh->execModel == ExecMesh) {
        /* The records and the two GIF packets end VU1 data memory; a vertex is [ST,] RGBAQ, [FOG,] XYZ2. */
        int out = 2 + (sh->texcoordVar ? 1 : 0) + (sh->fogVar ? 1 : 0), bytes;

        sh->vertQwords = sh->primQwords = 1;
        if (sh->colorVar && !sh->meshColorPerPrim)
            sh->colorSlot = sh->vertQwords++;
        if (sh->texcoordVar)
            sh->texcoordSlot = sh->vertQwords++;
        if (sh->fogVar)
            sh->fogSlot = sh->vertQwords++;
        if (sh->meshColorPerPrim)
            sh->colorSlot = sh->primQwords++;
        sh->kickVertices = (PVK_VU1_MESH_KICK_QWORDS - 1) / (sh->meshPrimVertices * out) * sh->meshPrimVertices;
        sh->kickQwords = 1 + sh->kickVertices * out;
        sh->kickA = PVK_VU1_MEMORY_QWORDS - sh->kickQwords;
        sh->kickB = sh->kickA - sh->kickQwords;
        sh->primBase = sh->kickB - (int)sh->meshMaxPrimitives * sh->primQwords;
        sh->vertBase = sh->primBase - (int)sh->meshMaxVertices * sh->vertQwords;
        bytes = (sh->kickB - sh->vertBase) * 16;
        if (bytes > PVK_MAX_MESH_OUTPUT_BYTES - sh->payloadQwords * 16)
            fail("mesh shader's %u vertices of %d bytes and %u primitives of %d bytes take %d bytes of VU1 "
                 "memory; they have %d (maxMeshOutputMemorySize) less the task payload's %d",
                 (unsigned)sh->meshMaxVertices, sh->vertQwords * 16, (unsigned)sh->meshMaxPrimitives,
                 sh->primQwords * 16, bytes, PVK_MAX_MESH_OUTPUT_BYTES, sh->payloadQwords * 16);
        g_synBase[SYN_VERT - SYN_VERT] = sh->vertBase;
        g_synBase[SYN_PRIM - SYN_VERT] = sh->primBase;
        g_synBase[SYN_COUNTS - SYN_VERT] = PVK_VU1_MESH_OUTPUTS;
        g_synBase[SYN_SHARED - SYN_VERT] = sh->vertBase - sh->sharedQwords;
        if (g_synBase[SYN_SHARED - SYN_VERT] < PVK_VU1_PAYLOAD_ADDR + sh->payloadQwords)
            fail("mesh shader's shared variables, outputs and task payload take %d bytes of VU1 memory "
                 "together; they have %d (PVK_MAX_MESH_MEMORY_BYTES less the two GIF packets)",
                 (sh->sharedQwords + sh->payloadQwords) * 16 + bytes, (sh->kickB - PVK_VU1_PAYLOAD_ADDR) * 16);
    }
    if (sh->execModel == ExecGLCompute) {
        uint32_t bytes = 0;
        for (int i = 0; i < sh->uniformCount; i++)
            bytes += (sh->uniforms[i].bytes + 15) / 16 * 16;
        for (int i = 0; i < sh->storageCount; i++)
            bytes += (sh->storage[i].bytes + 15) / 16 * 16;
        if (bytes > PVK_MAX_STORAGE_BYTES)
            fail("the compute shader's uniform and storage blocks take %u bytes together; a dispatch has %d "
                 "bytes of VU1 memory for them (maxStorageBufferRange)", (unsigned)bytes, PVK_MAX_STORAGE_BYTES);
    }

    /* Pass 2: the function body. */
    inFunction = 0;
    for (pc = 5; pc < words;) {
        uint32_t op = code[pc] & 0xffff, n = code[pc] >> 16, fake[16];
        const uint32_t *w = &code[pc];
        const char *stage = sh->execModel == ExecFragment ? "fragment" : g_stage ? g_stage : "vertex";
        int replay = labels && replayed < specOpCount;

        if (replay) {
            /* An operation on specialization constants, as the same body op at the
             * start of the body: its operands are only known at pipeline creation. */
            const uint32_t *so = specOps[replayed++];
            n = (so[0] >> 16) - 1;
            op = so[3];
            if (n < 3 || n > 16)
                fail("corrupt SPIR-V: OpSpecConstantOp with %u words", (unsigned)n);
            fake[0] = (n << 16) | op;
            fake[1] = so[1];
            fake[2] = so[2];
            memcpy(&fake[3], &so[4], (n - 3) * sizeof(uint32_t));
            w = fake;
        }
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
            case OpLoopMerge: case OpUnreachable:
                break;
            case OpSelectionMerge:
                sh->branchMerge = w[1];
                break;
            case OpKill: case OpTerminateInvocation: case OpDemoteToHelperInvocation:
                if (sh->execModel != ExecFragment)
                    unsupported(OpKill, stage);
                if (sh->discard)
                    fail("fragment shader has more than one discard; the GS has one alpha test per draw");
                if (!sh->branches) {
                    sh->discard = DISCARD_ALWAYS;
                } else if (sh->lastLabel == sh->branchTrue && sh->branchTrue != sh->branchMerge) {
                    sh->discard = DISCARD_IF;
                } else if (sh->lastLabel == sh->branchFalse && sh->branchFalse != sh->branchMerge) {
                    sh->discard = DISCARD_IF;
                    sh->discardInv = 1;
                } else {
                    fail("fragment shader discards outside its 'if'; " DISCARD_FORM);
                }
                /* A store in the block that discards is never seen. */
                if (op != OpDemoteToHelperInvocation && sh->storeLabel == sh->lastLabel) {
                    sh->colorValue = sh->prevColorValue;
                    sh->textureFunction = sh->prevTextureFunction;
                    sh->fogged = sh->prevFogged;
                }
                break;
            case OpLabel:
                new_op(sh->body, &sh->bodyCount, OpLabel, w[1]);
                sh->lastLabel = w[1];
                labels++;
                break;
            case OpReturn:
                sh->returns++;
                break;
            case OpControlBarrier:
                if (sh->execModel != ExecMesh && sh->execModel != ExecTask)
                    unsupported(op, stage);
                new_op(sh->body, &sh->bodyCount, OpBarrier, 0);
                sh->barriers++;
                break;
            case OpMemoryBarrier:
                /* VU1 runs one invocation at a time: the next read sees every store before it. */
                if (sh->execModel != ExecMesh && sh->execModel != ExecTask)
                    unsupported(op, stage);
                break;
            case OpEmitMeshTasksEXT: {
                /* Ends the invocation like a return; the group's first call counts. */
                uint32_t g[3];
                BodyOp *b;

                if (sh->execModel != ExecTask)
                    unsupported(op, stage);
                for (int k = 0; k < 3; k++) {
                    g[k] = int_operand(ids, sh, w[1 + k], sh->body, &sh->bodyCount);
                    if (value_size(ids, g[k], stage) != 1)
                        fail("corrupt SPIR-V: EmitMeshTasksEXT of something other than three counts");
                }
                b = new_op(sh->body, &sh->bodyCount, OpEmitTasks, 0);
                for (int k = 0; k < 3; k++)
                    b->ops[b->nops++] = g[k];
                sh->returns++;
                break;
            }
            case OpSetMeshOutputsEXT: {
                uint32_t counts = new_id(sh);

                if (sh->execModel != ExecMesh)
                    unsupported(op, stage);
                ids[counts].kind = ID_PTR;
                ids[counts].storBlock = SYN_COUNTS + 1;
                for (uint32_t k = 0; k < 2; k++) {
                    if (!is_int(ids, ids[w[1 + k]].valType))
                        fail("corrupt SPIR-V: SetMeshOutputsEXT of something other than two counts");
                    storage_store(ids, sh, counts, ids[w[1 + k]].valType, 4 * k, w[1 + k], stage);
                }
                break;
            }
            case OpBranch: {
                BodyOp *b = new_op(sh->body, &sh->bodyCount, OpBranch, 0);
                b->target = w[1];
                break;
            }
            case OpBranchConditional: {
                BodyOp *b;
                if (sh->execModel == ExecFragment) {
                    if (!ids[w[1]].fragCmp)
                        unsupported(op, stage);
                    if (sh->branches++)
                        fail("fragment shader has more than one 'if'; the GS has one alpha test per draw");
                    sh->branchLine  = g_line;
                    sh->branchCond  = w[1];
                    sh->branchTrue  = w[2];
                    sh->branchFalse = w[3];
                    break;
                }
                if (!IS_CODE(sh))
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

                if (!IS_CODE(sh))
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

                if (sh->execModel == ExecFragment) {
                    /* alpha CMP constant, whichever side the constant is on. */
                    int left = frag_alpha(ids, sh, w[3]), right = frag_alpha(ids, sh, w[4]);
                    uint32_t k = left ? w[4] : w[3];
                    int cmp = frag_compare(op, !left);

                    if (!cmp || !left == !right || ids[k].kind != ID_CONST || ids[k].compCount != 1 ||
                        !is_float(ids, ids[k].valType))
                        fail("fragment shader compares something other than the alpha of its color with a "
                             "constant; " DISCARD_FORM);
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].fragAlpha = left ? left : right;
                    ids[id].fragCmp = cmp;
                    memcpy(&ids[id].fragRef, &ids[k].comps[0], sizeof(float));
                    break;
                }
                if (!IS_CODE(sh))
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
                if (!IS_CODE(sh))
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
                if (sh->execModel == ExecFragment && ids[w[3]].fragCmp) {
                    ids[w[2]] = ids[w[3]];
                    ids[w[2]].fragCmp = frag_not(ids[w[3]].fragCmp);
                    break;
                }
                if (!IS_CODE(sh))
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

                if (!IS_CODE(sh))
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
                ids[id].comp = size == 1 ? scalar_comp(ids, sh->constBase, a) : 0;
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
                if (ids[ptr].builtinQword) {
                    /* A gl_ built-in: the microcode keeps it as floats in an invocation qword. */
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].fromConst = ids[ptr].builtinQword - 1;
                    ids[id].intIsFloat = 1;
                    if (ids[ptr].kind == ID_PTR)
                        ids[id].comp = ids[ptr].comp;
                    else if (ids[ptr].builtin == BuiltInLocalInvocationIndex)
                        ids[id].comp = 3;
                    else
                        ids[id].builtinQword = ids[ptr].builtinQword;
                    break;
                }
                if (ids[ptr].kind == ID_VAR && ids[ptr].sharedQword)
                    ptr = shared_ptr(ids, sh, ptr);
                if (ids[ptr].kind == ID_PTR && ids[ptr].storBlock) {
                    uint32_t type = ids[ptr].ptrType;
                    if (ids[ptr].storBlock == SYN_SHARED + 1 && (ids[type].type == T_STRUCT || ids[type].type == T_ARRAY)) {
                        /* A whole shared struct or array: a value per member or element, a qword each. */
                        int isStruct = ids[type].type == T_STRUCT;
                        int count = isStruct ? ids[type].memberCount : (int)ids[type].elemCount;

                        if (count > MAX_STRUCT_MEMBERS)
                            fail("compute shader reads a shared struct or array of %d members or elements at once; "
                                 "VU1 has registers for %d, so read them one at a time", count, MAX_STRUCT_MEMBERS);
                        for (int m = 0; m < count; m++) {
                            uint32_t mt = isStruct ? ids[type].members[m] : ids[type].elemType, mid = new_id(sh);
                            if (!is_int(ids, mt) && !type_size(ids, mt))
                                fail("compute shader reads a whole shared struct or array that holds a matrix, "
                                     "struct or array; read those members one at a time");
                            storage_load(ids, sh, mid, mt, ptr, 16 * (uint32_t)m);
                            ids[id].smem[m] = mid;
                        }
                        ids[id].kind = ID_VALUE;
                        ids[id].valType = w[1];
                        ids[id].smemCount = count;
                        break;
                    }
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    if (matrix_cols(ids, type)) {
                        /* A whole matrix: a value per column, in consecutive ids. */
                        int cols = matrix_cols(ids, type);
                        uint32_t col0 = new_id(sh);
                        for (int c = 1; c < cols; c++)
                            new_id(sh);
                        ids[id].dynCol0 = col0;
                        for (int c = 0; c < cols; c++)
                            storage_load(ids, sh, col0 + (uint32_t)c, ids[type].elemType, ptr, 16 * (uint32_t)c);
                    } else if (ids[type].type == T_STRUCT) {
                        if (ids[type].memberCount > MAX_STRUCT_MEMBERS)
                            fail("compute shader reads a struct of more than %d members from a storage block at "
                                 "once; read its members one at a time", MAX_STRUCT_MEMBERS);
                        for (int m = 0; m < ids[type].memberCount; m++) {
                            uint32_t mt = ids[type].members[m], mid = new_id(sh);
                            if (!is_int(ids, mt) && !type_size(ids, mt))
                                fail("compute shader reads a whole struct holding a matrix, struct or array from "
                                     "a storage block; read its members one at a time");
                            storage_load(ids, sh, mid, mt, ptr, member_offset(type, (uint32_t)m));
                            ids[id].smem[m] = mid;
                        }
                        ids[id].smemCount = ids[type].memberCount;
                    } else {
                        storage_load(ids, sh, id, w[1], ptr, 0);
                    }
                    break;
                }
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
                if (ids[ptr].kind == ID_PTR && ids[ptr].fragAlpha) {
                    /* outColor.a read back */
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].fragAlpha = ids[ptr].fragAlpha;
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

                if (!IS_CODE(sh))
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

                if (!IS_CODE(sh))
                    unsupported(op, stage);
                if (value_size(ids, src, stage) != 1)
                    fail("corrupt SPIR-V: int() of something other than a float");
                if (ids[src].kind == ID_CONST && !is_spec(&ids[src])) {
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

                if (!IS_CODE(sh))
                    unsupported(op, stage);
                if (ids[src].kind == ID_VALUE && ids[src].smemCount) {
                    /* q.vel = v or q.vel.x = f: the struct with one member replaced. */
                    uint32_t k = w[5];
                    ids[id] = ids[src];
                    ids[id].name = NULL;
                    ids[id].valType = w[1];
                    if (k >= (uint32_t)ids[src].smemCount || n > 7)
                        fail("compute shader writes a member of a struct member; only struct.member and "
                             "struct.member.x are supported");
                    if (n == 6) {
                        ids[id].smem[k] = obj;
                    } else {
                        uint32_t mv = ids[src].smem[k], nid = new_id(sh);
                        Field fl[4];
                        size = value_size(ids, mv, stage);
                        if (w[6] >= (uint32_t)size || value_size(ids, obj, stage) != 1)
                            fail("corrupt SPIR-V: a struct member's component out of range");
                        for (int j = 0; j < size; j++)
                            fl[j] = field_of(ids, sh, j == (int)w[6] ? obj : mv, j == (int)w[6] ? 0 : j);
                        define_map(ids, sh, op, nid, ids[mv].valType, fl, size);
                        ids[id].smem[k] = nid;
                    }
                    break;
                }
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
                    if (!IS_CODE(sh))
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
                if (!IS_CODE(sh))
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
                    ids[w[2]].comp = op == OpFDiv ? 0 : scalar_comp(ids, sh->constBase, a);
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

                if (!IS_CODE(sh))
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

                if (sh->execModel == ExecFragment && op == OpExtInst && w[3] == sh->glsl450 && ext == GlslFMix &&
                    nargs == 3) {
                    /* mix(<constant vec3>, <color>.rgb, <fog>) */
                    uint32_t k = args[0], rgb = args[1], amount = args[2];

                    if (ids[k].kind != ID_CONST || ids[k].compCount != 3 || ids[rgb].fragKind != FK_RGB ||
                        !(ids[amount].fragKind == FK_FOG_VEC || frag_is_fog(ids, sh, amount)))
                        fail("fragment shader's mix() is not the fog mix; " FOG_FORM);
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].fragKind = FK_FOGGED;
                    ids[id].fragClass = ids[rgb].fragClass;
                    ids[id].fragSrc = k;
                    break;
                }
                if (!IS_CODE(sh))
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
                        ids[id].comp = ext == GlslSqrt || ext == GlslInverseSqrt ? 0
                                     : scalar_comp(ids, sh->constBase, args[0]);
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

                if (sh->execModel == ExecFragment && ids[src].fragKind == FK_FOGGED && n == 5 && w[4] < 3) {
                    /* one of the mix's three components, on its way into the vec4 */
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].fragKind = FK_FOGGED_COMP;
                    ids[id].fragSrc = src;
                    ids[id].comp = (int)w[4];
                    break;
                }
                if (sh->execModel == ExecFragment) {
                    int whose = frag_color(ids, sh, src);

                    if (!whose || n != 5 || w[4] != 3)
                        fail("fragment shader reads something other than the alpha of its color; " DISCARD_FORM);
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].fragAlpha = whose;
                    break;
                }
                if (!IS_CODE(sh))
                    unsupported(op, stage);
                if (ids[src].kind == ID_VALUE && ids[src].builtinQword) {
                    if (n != 5 || w[4] >= 3)
                        fail("corrupt SPIR-V: a component of a gl_ built-in out of range");
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].fromConst = ids[src].builtinQword - 1;
                    ids[id].comp = (int)w[4];
                    ids[id].intIsFloat = 1;
                    break;
                }
                if (ids[src].kind == ID_VALUE && ids[src].smemCount) {
                    /* q.vel or q.vel.x of a struct value: its member's fields. */
                    uint32_t mv;
                    Field fl[4];
                    if (n < 5 || n > 6 || w[4] >= (uint32_t)ids[src].smemCount)
                        fail("compute shader reads a member of a struct member; only struct.member and "
                             "struct.member.x are supported");
                    mv = ids[src].smem[w[4]];
                    size = n == 6 ? 1 : value_size(ids, mv, stage);
                    for (int j = 0; j < size; j++)
                        fl[j] = field_of(ids, sh, mv, n == 6 ? (int)w[5] : j);
                    define_map(ids, sh, op, id, w[1], fl, size);
                    if (ids[id].kind == ID_VALUE && ids[mv].kind == ID_VALUE)
                        ids[id].intIsFloat = ids[mv].intIsFloat;
                    break;
                }
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

                if (sh->execModel == ExecFragment) {
                    /* <color>.rgb */
                    int whose = frag_color(ids, sh, v1);

                    if (!whose || v2 != v1 || size != 3 || w[5] != 0 || w[6] != 1 || w[7] != 2 ||
                        ids[v1].fragFogged)
                        fail("fragment shader swizzles something other than the .rgb of its color; " FOG_FORM);
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].fragKind = FK_RGB;
                    ids[id].fragClass = whose;
                    break;
                }
                if (!IS_CODE(sh))
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

                if (sh->execModel == ExecFragment) {
                    uint32_t a = w[3], mix = 0, alpha = 0;

                    if (n == 6 && frag_is_fog(ids, sh, a) && w[4] == a && w[5] == a) {
                        /* vec3(<fog>): how glslang passes a float as mix()'s third argument */
                        ids[id].kind = ID_VALUE;
                        ids[id].valType = w[1];
                        ids[id].fragKind = FK_FOG_VEC;
                        break;
                    }
                    if (n == 5 && ids[a].fragKind == FK_FOGGED) {
                        mix = a;
                        alpha = w[4];
                    } else if (n == 7 && ids[a].fragKind == FK_FOGGED_COMP && ids[a].comp == 0 &&
                               ids[w[4]].fragKind == FK_FOGGED_COMP && ids[w[4]].comp == 1 &&
                               ids[w[5]].fragKind == FK_FOGGED_COMP && ids[w[5]].comp == 2 &&
                               ids[w[4]].fragSrc == ids[a].fragSrc && ids[w[5]].fragSrc == ids[a].fragSrc) {
                        mix = ids[a].fragSrc;
                        alpha = w[6];
                    }
                    if (!mix || frag_alpha(ids, sh, alpha) != ids[mix].fragClass)
                        fail("fragment shader builds a vector other than the fogged color; " FOG_FORM);
                    frag_define_fogged(ids, sh, id, w[1], ids[mix].fragClass);
                    ids[id].fragSrc = ids[mix].fragSrc;
                    break;
                }
                if (!IS_CODE(sh))
                    unsupported(op, stage);
                if (ids[w[1]].type == T_STRUCT && IS_GROUPED(sh)) {
                    /* A struct value is its members' values. */
                    if (n - 3 > MAX_STRUCT_MEMBERS)
                        fail("compute shader builds a struct of more than %d members", MAX_STRUCT_MEMBERS);
                    ids[id].kind = ID_VALUE;
                    ids[id].valType = w[1];
                    ids[id].smemCount = (int)(n - 3);
                    for (uint32_t i = 0; i < n - 3; i++)
                        ids[id].smem[i] = w[3 + i];
                    break;
                }
                size = type_size(ids, w[1]);
                if (ids[w[1]].type == T_MATRIX)
                    fail("vertex shader builds a matrix from values computed at run time; "
                         "only push constant, uniform and constant matrices are supported so far");
                if (!size || n - 3 > 4)
                    fail("vertex shader constructs something other than a vec2, vec3 or vec4 "
                         "from floats and vectors; only those are supported so far");
                for (uint32_t i = 0; i < n - 3; i++) {
                    /* An int vector holds the floats VU1 counts with. */
                    uint32_t o = is_int_vector(ids, w[1]) ? int_operand(ids, sh, w[3 + i], sh->body, &sh->bodyCount)
                                                          : w[3 + i];
                    int k = value_size(ids, o, stage);
                    if (total + k > size)
                        fail("corrupt SPIR-V: OpCompositeConstruct operands overflow the result");
                    for (int j = 0; j < k; j++)
                        fl[total + j] = field_of(ids, sh, o, j);
                    total += k;
                }
                if (total != size)
                    fail("corrupt SPIR-V: OpCompositeConstruct operands do not fill the result");
                define_map(ids, sh, op, id, w[1], fl, size);
                break;
            }
            case OpAccessChain: case OpInBoundsAccessChain: {
                uint32_t id = w[2], base = w[3], blockBytes = 0;
                int blockQword = 0;
                const char *kind = const_block(sh, base, &blockQword, &blockBytes);
                if (is_plain_base(ids, sh, base)) {
                    plain_chain(ids, sh, w, n);
                    break;
                }
                if (storage_index(sh, base) >= 0 || (ids[base].kind == ID_PTR && ids[base].storBlock)) {
                    storage_chain(ids, sh, w, n);
                    break;
                }
                if (ids[base].builtinQword) {
                    if (n != 5 || ids[w[4]].kind != ID_CONST || ids[w[4]].value >= 3)
                        fail("compute shader picks a component of a gl_ built-in with something other than a "
                             "constant");
                    ids[id].kind = ID_PTR;
                    ids[id].builtinQword = ids[base].builtinQword;
                    ids[id].comp = (int)ids[w[4]].value;
                    break;
                }
                if (kind) {
                    /* pc.member, pc.array[i], pc.member.z: a constant byte offset into
                     * the block, which VU1 memory holds as qwords. */
                    uint32_t type = ids[ids[base].varType].elemType, off = 0, dynIndex = 0;
                    int size, dynStride = 0, dynCount = 0;

                    for (uint32_t i = 4; i < n; i++) {
                        uint32_t idx = w[i], v;
                        if (ids[idx].kind == ID_CONST && is_spec(&ids[idx]))
                            fail("vertex shader indexes a %s with a specialization constant; the element's "
                                 "address is part of the code, so index with a fixed constant, or at run "
                                 "time with int() of a float", kind);
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
                    if (sh->execModel == ExecFragment &&
                        (ids[base].fromInput != sh->colorInput || n != 5 || ids[w[4]].kind != ID_CONST ||
                         ids[w[4]].value != 3))
                        fail("fragment shader reads something other than the alpha of its color; " DISCARD_FORM);
                    if (n != 5 || ids[w[4]].kind != ID_CONST || ids[w[4]].value >= 4)
                        fail("vertex shader indexes an input with something other than a "
                             "constant component; only in.x .. in.w is supported so far");
                    ids[id].kind = ID_PTR;
                    ids[id].fromInput = ids[base].fromInput;
                    ids[id].comp = (int)ids[w[4]].value;
                    break;
                }
                if (sh->execModel == ExecFragment && base && base == sh->colorVar) {
                    if (n != 5 || ids[w[4]].kind != ID_CONST || ids[w[4]].value != 3)
                        fail("fragment shader reads something other than the alpha of its color; " DISCARD_FORM);
                    ids[id].kind = ID_PTR;
                    ids[id].fragAlpha = ALPHA_OUTPUT;
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
                    sh->prevColorValue = sh->colorValue;
                    sh->prevTextureFunction = sh->textureFunction;
                    sh->prevFogged = sh->fogged;
                    sh->storeLabel = sh->lastLabel;
                    sh->fogged = ids[val].fragFogged;
                    if (sh->fogged)
                        sh->fogColor = frag_fogcol(&ids[ids[val].fragSrc]);
                    sh->colorValue = val;
                    sh->textureFunction = ids[val].texel == TEXEL_SAMPLE ? 1 : 0;
                    break;
                }
                if (ids[ptr].kind == ID_PTR && ids[ptr].ignored)
                    break;
                if (ids[ptr].kind == ID_PTR && ids[ptr].cullFlag) {
                    cull_store(ids, sh, ptr, val);
                    break;
                }
                if (ids[ptr].kind == ID_VAR && ids[ptr].sharedQword)
                    ptr = shared_ptr(ids, sh, ptr);
                if (is_plain_base(ids, sh, ptr) && ids[ptr].kind != ID_PTR)
                    fail("compute shader stores a whole mesh output array or task payload; write its members "
                         "and elements one at a time");
                if (ids[ptr].kind == ID_PTR && ids[ptr].storBlock) {
                    storage_store(ids, sh, ptr, ids[ptr].ptrType, 0, val, stage);
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
                    } else if (ptr && ptr == sh->fogVar) {
                        if (value_size(ids, val, stage) != 1)
                            fail("vertex shader stores something other than a float to the fog output");
                        out = OUT_FOG;
                    } else {
                        fail("%s shader writes to something other than its outputs", stage);
                    }
                    b = new_op(sh->body, &sh->bodyCount, OpStore, 0);
                    b->outVar = out;
                    b->ops[b->nops++] = val;
                }
                break;
            }
            case OpArrayLength: {
                int b = storage_index(sh, w[3]);
                if (b < 0 || !sh->storage[b].arrayStride)
                    fail("corrupt SPIR-V: length() of something other than a storage block's runtime array");
                ids[w[2]].kind = ID_VALUE;
                ids[w[2]].valType = w[1];
                ids[w[2]].fromConst = LENGTHS_QWORD;
                ids[w[2]].comp = b;
                ids[w[2]].intIsFloat = 1;
                break;
            }
            case OpBitcast: case OpCopyObject:
                /* int <-> uint (length() comes as an int): the same value. */
                if (IS_GROUPED(sh) && ids[w[3]].kind == ID_VALUE &&
                    (op == OpCopyObject ? type_size(ids, w[1]) > 0
                                        : is_int(ids, w[1]) && is_int(ids, ids[w[3]].valType))) {
                    alias_value(ids, w[2], w[1], w[3]);
                    ids[w[2]].intIsFloat = ids[w[3]].intIsFloat;
                    ids[w[2]].floatIndex = ids[w[3]].floatIndex;
                    break;
                }
                unsupported(op, stage);
                break;
            default:
                unsupported(op, stage);
            }
        } else if (op == OpSpecConstantOp) {
            if (specOpCount == MAX_SPEC_OPS)
                fail("shader has more than %d operations on specialization constants", MAX_SPEC_OPS);
            specOps[specOpCount++] = w;
        }
        if (!replay)
            pc += n;
    }
    g_line = 0;

    if (IS_CODE(sh)) {
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
        if (sh->fogValue)
            used[root(ids, sh->fogValue)] = 1;
        for (int i = 0; i < sh->bodyCount; i++)
            if (is_map_op(sh->body[i].op) && !used[sh->body[i].result])
                sh->body[i].dead = 1;
    }

    if (IS_CODE(sh)) {
        place_const_loads(ids, sh);
        /* A scalar 0 or 1 reads VF00 = (0, 0, 0, 1) instead of taking a register. */
        for (uint32_t i = 0; i < sh->constBase; i++) {
            if (ids[i].kind != ID_CONST || ids[i].compCount != 1 || is_spec(&ids[i]))
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

    if (sh->execModel == ExecMesh && (!sh->meshVertVar || !sh->meshIndexVar))
        fail("mesh shader never writes %s", sh->meshVertVar ? "its primitives' indices (gl_PrimitiveTriangleIndicesEXT, "
             "gl_PrimitiveLineIndicesEXT or gl_PrimitivePointIndicesEXT)" : "gl_MeshVerticesEXT[].gl_Position");
    if (sh->execModel == ExecVertex) {
        if (!sh->positionValue)
            fail("vertex shader never writes gl_Position");
        if (sh->colorVar && !sh->colorValue)
            fail("vertex shader declares a color output but never writes it");
        if (sh->texcoordVar && !sh->texcoordValue)
            fail("vertex shader declares a texture coordinate output but never writes it");
        if (sh->fogVar && !sh->fogValue)
            fail("vertex shader declares a fog output but never writes it");
    } else if (sh->execModel == ExecFragment) {
        g_line = sh->branchLine;
        if (sh->branches && sh->discard != DISCARD_IF)
            fail("fragment shader has an 'if' that does not discard: the GS cannot run code per pixel");
        if (!sh->colorValue && sh->discard != DISCARD_ALWAYS)
            fail("fragment shader must write 'outColor = <color input>;' or "
                 "'outColor = texture(<sampler>, <texcoord input>);'");
        if (sh->discard == DISCARD_IF) {
            int tested = ids[sh->branchCond].fragAlpha, written = frag_color(ids, sh, sh->colorValue);

            if (tested != ALPHA_OUTPUT && tested != written)
                fail("fragment shader discards on the alpha of %s but writes %s; " DISCARD_FORM,
                     frag_color_name(tested), frag_color_name(written));
        }
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

/* Writes <base>.pvs: the header, the two names, the clamp offsets, the specialization
 * constants and the microcode, in that order. `code` is NULL for a fragment shader. */
static void write_pvs(const char *base, PvkPvsHeader *h, const char *name, const char *entry,
                      const uint32_t *code, const uint32_t *clamps, const PvkPvsSpecConstant *specs)
{
    static const char zero[4];
    char path[1024];
    size_t nameLen = strlen(name) + 1, entryLen = strlen(entry) + 1;
    size_t strings = (nameLen + entryLen + 3) & ~(size_t)3;
    FILE *f;

    h->magic            = PVK_PVS_MAGIC;
    h->version          = PVK_PVS_VERSION;
    h->nameOffset       = (uint32_t)sizeof(*h);
    h->entryPointOffset = h->nameOffset + (uint32_t)nameLen;
    h->clampsOffset     = h->nameOffset + (uint32_t)strings;
    h->specsOffset      = h->clampsOffset + h->clampCount * 4;
    h->codeOffset       = h->specsOffset + h->specCount * (uint32_t)sizeof(*specs);
    if (!code)
        h->codeWords = 0;
    h->fileBytes        = h->codeOffset + h->codeWords * 4;

    snprintf(path, sizeof(path), "%s.pvs", base);
    f = fopen(path, "wb");
    if (!f)
        fail("cannot write %s", path);
    fwrite(h, sizeof(*h), 1, f);
    fwrite(name, 1, nameLen, f);
    fwrite(entry, 1, entryLen, f);
    fwrite(zero, 1, strings - nameLen - entryLen, f);
    if (h->clampCount)
        fwrite(clamps, 4, h->clampCount, f);
    if (h->specCount)
        fwrite(specs, sizeof(*specs), h->specCount, f);
    if (h->codeWords)
        fwrite(code, 4, h->codeWords, f);
    if (ferror(f) || fclose(f))
        fail("cannot write %s", path);
}

/* A whole file in memory, 4 zero bytes after it. */
static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long n;

    if (!f)
        fail("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = calloc((size_t)n + 4, 1);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n)
        fail("cannot read %s", path);
    fclose(f);
    *size = (size_t)n;
    return data;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* The 32-bit ELF object dvp-as wrote: its .vutext section and its symbols. */
typedef struct {
    const uint8_t *data, *text, *syms, *strs;
    size_t size, textSize, symCount, strSize;
    uint32_t textIndex;
} VuObject;

static void read_vu_object(const char *path, VuObject *o)
{
    const uint8_t *sh, *names;
    uint32_t shoff, shentsize, shnum, shstrndx;

    memset(o, 0, sizeof(*o));
    o->data = read_file(path, &o->size);
    if (o->size < 52 || memcmp(o->data, "\177ELF\1\1", 6))
        fail("%s is not a 32-bit little-endian ELF object from dvp-as", path);
    shoff     = rd32(o->data + 32);
    shentsize = rd16(o->data + 46);
    shnum     = rd16(o->data + 48);
    shstrndx  = rd16(o->data + 50);
    if (shentsize < 40 || shstrndx >= shnum || shoff + (size_t)shnum * shentsize > o->size)
        fail("%s: corrupt ELF section table", path);
    names = o->data + rd32(o->data + shoff + shstrndx * shentsize + 16);
    for (uint32_t i = 0; i < shnum; i++) {
        uint32_t type, off, size, link;

        sh   = o->data + shoff + i * shentsize;
        type = rd32(sh + 4);
        off  = rd32(sh + 16);
        size = rd32(sh + 20);
        link = rd32(sh + 24);
        if (type != 8 /* NOBITS */ && off + (size_t)size > o->size)
            fail("%s: corrupt ELF section", path);
        if (!strcmp((const char *)names + rd32(sh), ".vutext")) {
            o->text      = o->data + off;
            o->textSize  = size;
            o->textIndex = i;
        } else if (type == 2 /* SYMTAB */ && link < shnum) {
            const uint8_t *str = o->data + shoff + link * shentsize;

            o->syms     = o->data + off;
            o->symCount = size / 16;
            o->strs     = o->data + rd32(str + 16);
            o->strSize  = rd32(str + 20);
        }
    }
    if (!o->text || !o->syms)
        fail("%s has no .vutext section or no symbols; assemble the .vsm with dvp-as", path);
}

/* Byte offset in .vutext of the label <sym><suffix>. */
static uint32_t vu_label(const char *path, const VuObject *o, const char *sym, const char *suffix)
{
    char name[320];

    snprintf(name, sizeof(name), "%s%s", sym, suffix);
    for (size_t i = 0; i < o->symCount; i++) {
        const uint8_t *s = o->syms + i * 16;
        uint32_t n = rd32(s);

        if (n < o->strSize && rd16(s + 14) == o->textIndex && !strcmp((const char *)o->strs + n, name)) {
            if (rd32(s + 4) > o->textSize || rd32(s + 4) % 4)
                fail("%s: label %s lies outside the microcode", path, name);
            return rd32(s + 4);
        }
    }
    fail("%s has no label %s; it was not assembled from this shader's .vsm", path, name);
    return 0;
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
static const int g_vertexPool[] = { 5, 6, 7, 8, 9, 18, 19, 27, 28, 29, 30, 31 };
/* A compute shader has no inputs or epilogue: VF01-VF04 hold the loop state, the rest is free. */
static const int g_computePool[] = { 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
                                     25, 26, 27, 28, 29, 30, 31 };
static const int *g_pool = g_vertexPool;
static int g_poolCount = (int)(sizeof(g_vertexPool) / sizeof(g_vertexPool[0]));
/* Mesh: a constant is loaded where a block first reads it and its register given back after the
 * block's last read, since a loop written out per vertex has more constants than VU1 has registers. */
static int g_lazyConst = 0;

typedef struct {
    int reg[MAX_IDS];       /* VF register holding each value, 0 if none */
    int lastUse[MAX_IDS];   /* body index of the last read, MAX_BODY if stored to an output */
    int busy[32];
} Regs;

static int reg_alloc(Regs *r)
{
    for (int i = 0; i < g_poolCount; i++)
        if (!r->busy[g_pool[i]]) {
            r->busy[g_pool[i]] = 1;
            return g_pool[i];
        }
    fail("vertex shader needs more than %d values at once; VU1 has run out of registers", g_poolCount);
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
    if (g_lazyConst && ids[id].kind == ID_CONST && r->reg[id]) {
        for (int k = at + 1; k < sh->bodyCount && sh->body[k].op != OpLabel; k++)
            for (int j = 0; j < sh->body[k].nops && !sh->body[k].dead; j++)
                if (canon(ids, sh, sh->body[k].ops[j]) == id)
                    return;
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

    static const struct { int addr; const char *what; } compute[] = {
        { PVK_VU1_HEADER_GROUPS, "gl_NumWorkGroups" },
        { PVK_VU1_HEADER_WORKGROUP, "gl_WorkGroupID" },
        { PVK_VU1_HEADER_LOCAL, "gl_LocalInvocationID, w = gl_LocalInvocationIndex" },
        { PVK_VU1_HEADER_GLOBAL, "gl_GlobalInvocationID" },
        { PVK_VU1_HEADER_LENGTHS, "length() of each storage block's runtime array" },
    };

    if (q > INSTANCE_QWORD) {
        snprintf(buf, sizeof(buf), "lq            VF%02d,%d(VI00)", reg, hdr_addr(compute[q - GROUPS_QWORD].addr));
        snprintf(buf2, sizeof(buf2), "%s", compute[q - GROUPS_QWORD].what);
    } else if (q == INSTANCE_QWORD) {
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
static void emit_constant(FILE *f, const IdInfo *ids, const IdInfo *c, int reg, const char *sym);

static void reg_ensure_loaded(FILE *f, Regs *r, const IdInfo *ids, const Shader *sh, uint32_t id)
{
    uint32_t c = canon(ids, sh, id);

    if (g_lazyConst && ids[c].kind == ID_CONST && !ids[c].vf00 && !r->reg[c]) {
        r->reg[c] = reg_alloc(r);
        emit_constant(f, ids, &ids[c], r->reg[c], g_sym);
    } else if (c >= sh->constBase && ids[c].kind != ID_CONST && !r->reg[c]) {
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

/* PvkSpecConstantType of a specialization constant of that type, or a vector of it. */
static int spec_type(const IdInfo *ids, uint32_t type)
{
    if (ids[type].type == T_VECTOR)
        type = ids[type].elemType;
    if (ids[type].type == T_BOOL)
        return PVK_SPEC_CONSTANT_TYPE_BOOL;
    if (ids[type].type == T_INT)
        return ids[type].storage ? PVK_SPEC_CONSTANT_TYPE_INT : PVK_SPEC_CONSTANT_TYPE_UINT;
    return PVK_SPEC_CONSTANT_TYPE_FLOAT;
}

static const char *const spec_type_names[] = {
    "PVK_SPEC_CONSTANT_TYPE_FLOAT", "PVK_SPEC_CONSTANT_TYPE_INT", "PVK_SPEC_CONSTANT_TYPE_UINT",
    "PVK_SPEC_CONSTANT_TYPE_BOOL"
};

/* Loads a constant into its register: zero it, then LOI + ADDI per distinct
 * component value. A specialization constant's component gets a labelled LOI
 * of its own, whose immediate the pipeline overwrites. */
static void emit_constant(FILE *f, const IdInfo *ids, const IdInfo *c, int reg, const char *sym)
{
    static const char field[] = "xyzw";
    char buf[64], buf2[96], mask[5], val[32];
    int done[4] = { 0 }, i, j, k;

    snprintf(buf, sizeof(buf), "sub.xyzw      VF%02d,VF00,VF00", reg);
    snprintf(buf2, sizeof(buf2), "%s", c->compCount == 1 ? "float constant" : "vec4 constant");
    vu(f, buf, "NOP", buf2);
    for (i = 0; i < c->compCount; i++) {
        if (done[i] || (c->specId[i] < 0 && !(c->comps[i] & 0x7fffffff)))
            continue;
        loi_text(c->comps[i], buf, sizeof(buf), val, sizeof(val));
        if (c->specId[i] >= 0) {
            if (g_specCount == MAX_SPECS)
                fail("vertex shader uses more than %d specialization constants", MAX_SPECS);
            g_specs[g_specCount].id = c->specId[i];
            g_specs[g_specCount].type = spec_type(ids, c->valType);
            fprintf(f, "%s_Spec%d:\n", sym, g_specCount++);
            mask[0] = field[i];
            mask[1] = 0;
            done[i] = 1;
            snprintf(buf2, sizeof(buf2), "specialization constant %d, default %s", c->specId[i], val);
            vu(f, "NOP", buf, buf2);
        } else {
            for (j = i, k = 0; j < c->compCount; j++)
                if (c->comps[j] == c->comps[i] && c->specId[j] < 0) {
                    mask[k++] = field[j];
                    done[j] = 1;
                }
            mask[k] = 0;
            vu(f, "NOP", buf, val);
        }
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
            snprintf(buf, sizeof(buf), "maxw.%-8s VF%02d,VF00,VF00w", mask, d);
            vu(f, buf, "NOP", first ? line : NULL);
        } else if (ident) {
            snprintf(buf, sizeof(buf), "move.%-8s VF%02d,VF%02d", mask, d, regs[op]);
            vu(f, "NOP", buf, first ? line : NULL);
        } else {
            /* VF00.w is 1: x, y, z take 0 + the source field, w takes 1 * it. */
            char mn[16], xyz[5];
            int n = 0;
            for (const char *m = mask; *m; m++)
                if (*m != 'w')
                    xyz[n++] = *m;
            xyz[n] = 0;
            if (n) {
                snprintf(mn, sizeof(mn), "add%c.%s", field[sc], xyz);
                snprintf(buf, sizeof(buf), "%-14sVF%02d,VF00,VF%02d%c", mn, d, regs[op], field[sc]);
                vu(f, buf, "NOP", first ? line : NULL);
                first = 0;
            }
            if (strchr(mask, 'w')) {
                snprintf(mn, sizeof(mn), "mul%c.w", field[sc]);
                snprintf(buf, sizeof(buf), "%-14sVF%02d,VF00,VF%02d%c", mn, d, regs[op], field[sc]);
                vu(f, buf, "NOP", first ? line : NULL);
            }
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
    case OpStorLoad: return b->storBlock == SYN_SHARED ? "shared variable read"
                            : b->storBlock == SYN_PAYLOAD ? "task payload read" : b->storBlock >= SYN_VERT
                            ? "mesh output read" : "storage block read";
    case OpStorStore: return b->storBlock == SYN_SHARED ? "shared variable write"
                             : b->storBlock == SYN_PAYLOAD ? "task payload write" : b->storBlock == SYN_COUNTS
                             ? "SetMeshOutputsEXT" : b->storBlock >= SYN_VERT ? "mesh output write" : "storage block write";
    case OpEmitTasks: return "EmitMeshTasksEXT";
    case OpBarrier: return "barrier()";
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
    else /* VF00.w is 1: w takes 1 * the source field, x, y, z take 0 + it */
        emit_u(f, comment, mn(dc == 3 ? "mul" : "add", field[sc], m), "VF%02d,VF00,VF%02d%c", d, s, field[sc]);
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
            emit_u(f, &c, mn(ca == 3 ? "mul" : "add", field[cb], dest), "VF%02d,VF00,VF%02d%c", d, bb, field[cb]);
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

/*
 * The VI register a storage access goes through: the block's first qword (VI09 + block), or
 * VI08 = that + the run-time index times the element's qwords. The index is copied into field
 * dynComp of t, clamped to the array (to its last element, from the dispatch header, for a
 * runtime array), scaled and truncated there.
 */
static int emit_stor_addr(FILE *f, const char **c, const BodyOp *b, int t, int a)
{
    static const char field[] = "xyzw";
    const char *fm = &"x\0y\0z\0w"[b->dynComp * 2];
    char fc = field[b->dynComp];
    int base = PVK_VU1_STORAGE_BASE_REG + b->storBlock;
    int fixed = b->storBlock >= SYN_VERT;   /* a mesh output or the payload: dynBase holds its address */

    if (!b->storDyn)
        return fixed ? 0 : base;
    if (t != a)
        emit_move(f, c, t, b->dynComp, a, b->dynComp, 1);
    fprintf(f, "%s_Clamp%d:\n", g_sym, g_clamps++);
    emit_u(f, c, mn("maxx", 0, fm), "VF%02d,VF%02d,VF00x", t, t);
    if (b->storRuntime) {
        emit_l(f, c, "lq", "VF04,%d(VI00)", hdr_addr(PVK_VU1_HEADER_LAST));
        emit_u(f, c, mn("mini", field[b->storBlock], fm), "VF%02d,VF%02d,VF04%c", t, t, field[b->storBlock]);
    } else {
        emit_loi(f, c, b->dynLast);
        emit_u(f, c, mn("minii", 0, fm), "VF%02d,VF%02d,I", t, t);
    }
    if (b->dynStride > 1) {
        float s = (float)b->dynStride;
        uint32_t bits;
        memcpy(&bits, &s, sizeof(bits));
        emit_loi(f, c, bits);
        emit_u(f, c, mn("muli", 0, fm), "VF%02d,VF%02d,I", t, t);
    }
    emit_u(f, c, mn("ftoi0", 0, fm), "VF%02d,VF%02d", t, t);
    emit_l(f, c, "mtir", "VI08,VF%02d%c", t, fc);
    if (!fixed)
        emit_l(f, c, "iadd", "VI08,VI08,VI%02d", base);
    return 8;
}

/* EmitMeshTasksEXT(x, y, z): the mesh header's group counts, as floats and as ints, unless an
 * earlier invocation of the task work group set them; a count of 0 emits nothing. */
static void emit_tasks(FILE *f, const IdInfo *ids, const BodyOp *b, int t, const int *regs, const char *line)
{
    const char *c = line;
    int label = g_labels++;

    emit_l(f, &c, "ilw.w", "VI08,%d(VI00)", PVK_VU1_MESH_STATE);
    for (int k = 0; k < 3; k++)
        emit_move(f, &c, t, k, regs[k], ids[b->ops[k]].comp, 1);
    emit_l(f, &c, "ibne", "VI08,VI00,%s_E%d", g_sym, label);
    vu(f, "NOP", "NOP", "branch delay slot");
    emit_l(f, &c, "sq.xyz", "VF%02d,%d(VI00)", t, PVK_VU1_MESH_ADDR + PVK_VU1_HEADER_GROUPS);
    emit_u(f, &c, "ftoi0.xyz", "VF%02d,VF%02d", t, t);
    emit_l(f, &c, "sq.xyz", "VF%02d,%d(VI00)", t, PVK_VU1_MESH_ADDR + PVK_VU1_HEADER_COUNTS);
    for (int k = 0; k < 3; k++) {
        emit_l(f, &c, "mtir", "VI08,VF%02d%c", t, "xyz"[k]);
        vu(f, "NOP", "NOP", "the branch reads VI after one instruction");
        emit_l(f, &c, "ibeq", "VI08,VI00,%s_E%d", g_sym, label);
        vu(f, "NOP", "NOP", "branch delay slot");
    }
    emit_l(f, &c, "iaddiu", "VI08,VI00,1");
    emit_l(f, &c, "isw.w", "VI08,%d(VI00)", PVK_VU1_MESH_STATE);
    fprintf(f, "%s_E%d:\n", g_sym, label);
}

/* sq of value v (ops[0]) into fields storComp.. of a storage qword, through scratch t when the
 * value must move to those fields or become an integer. */
static void emit_stor_store(FILE *f, const IdInfo *ids, const BodyOp *b, int t, const int *regs, const char *line)
{
    static const char field[] = "xyzw";
    const char *c = line;
    int v = regs[0], vc = ids[b->ops[0]].comp, k = b->storComp, size = b->storSize, vi, src = v;
    char mask[5];

    vi = emit_stor_addr(f, &c, b, t, b->storDyn ? regs[1] : 0);
    for (int j = 0; j < size; j++)
        mask[j] = field[k + j];
    mask[size] = 0;
    if (size == 1 ? vc != k : k != 0) {
        for (int j = 0; j < size; j++)
            emit_move(f, &c, t, k + j, v, size == 1 ? vc : j, 1);
        src = t;
    }
    if (b->storToInt) {
        emit_u(f, &c, mn("ftoi0", 0, mask), "VF%02d,VF%02d", t, src);
        src = t;
    }
    emit_l(f, &c, mn("sq", 0, mask), "VF%02d,%d(VI%02d)", src, b->dynBase, vi);
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
    case OpStorLoad: {
        const char *c = line;
        int vi = emit_stor_addr(f, &c, b, d, a);
        emit_l(f, &c, "lq", "VF%02d,%d(VI%02d)", d, b->dynBase, vi);
        break;
    }
    }
}

/* barrier(): each invocation keeps g_ctxQwords qwords, a mesh shader's going down from its shared
 * variables, a task shader's going up from behind them. VI14 points at the running invocation's. */
static int ctx_start(const Shader *sh)
{
    return g_synBase[SYN_SHARED - SYN_VERT] + (sh->execModel == ExecMesh ? 0 : sh->sharedQwords);
}

/* Offset from VI14 of the invocation's qword k: 0 where it goes on, then its registers. */
static int ctx_off(const Shader *sh, int k)
{
    return sh->execModel == ExecMesh ? -1 - k : k;
}

/* barrier(): the registers in use go to the invocation's qwords and the next invocation runs;
 * a later pass over the group comes back behind the branch and loads them again. */
static void emit_barrier(FILE *f, const Regs *r, const IdInfo *ids, const Shader *sh, const char *line)
{
    int regs[32], n = 0;
    const char *c = line;

    for (int i = 0; i < g_poolCount; i++) {
        int reg = g_pool[i], constant = 0;

        if (!r->busy[reg])
            continue;
        /* A constant loaded for the whole program stays in its register. */
        for (uint32_t id = 0; id < MAX_IDS && !g_lazyConst && !constant; id++)
            constant = r->reg[id] == reg && ids[id].kind == ID_CONST;
        if (!constant)
            regs[n++] = reg;
    }
    if (1 + n > g_ctxQwords)
        g_ctxQwords = 1 + n;
    for (int k = 0; k < n; k++)
        emit_l(f, &c, "sq", "VF%02d,%d(VI14)", regs[k], ctx_off(sh, 1 + k));
    emit_l(f, &c, "bal", "VI13,%s_wait", g_sym);
    vu(f, "NOP", "NOP", "branch delay slot");
    for (int k = 0; k < n; k++)
        emit_l(f, &c, "lq", "VF%02d,%d(VI14)", regs[k], ctx_off(sh, 1 + k));
}

/* A compute, task or mesh program's start: the storage blocks' addresses and the local size. */
static void emit_compute_head(FILE *f, const Shader *sh, const char *sym)
{
    char buf[64], buf2[96], val[32];
    int task = sh->execModel == ExecTask, mesh = sh->execModel == ExecMesh;

    fprintf(f, "; Generated by ps2vk-shaderc from %s. Do not edit.\n", g_src);
    fprintf(f, ";\n; VU1 registers:\n");
    fprintf(f, ";   VF01 gl_WorkGroupID, VF02 gl_LocalInvocationID (w = gl_LocalInvocationIndex), VF03 local size\n");
    fprintf(f, ";   VF04 scratch, VF05-VF31 push constants, uniforms, storage values, constants and temporaries\n");
    if (mesh)
        fprintf(f, ";   geometry: VF24-VF26 positions, VF27-VF28 edges, VF29 winding sign, VF30 color, VF31 XYZ2\n");
    fprintf(f, ";   VI01-VI03 work groups left along x, y, z, VI04-VI06 invocations left along x, y, z\n");
    fprintf(f, ";   VI08 scratch, VI09-VI12 first qword of each storage block, VI13 scratch\n");
    if (mesh) {
        fprintf(f, ";   geometry: VI04 primitives left, VI05 record pointer, VI06 GIF packet pointer, VI07 scratch,\n");
        fprintf(f, ";   VI08, VI13, VI15 a primitive's vertex records, VI14 vertices left, then vertices in the packet\n");
    }
    fprintf(f, "\n\t\t.vu\n\t\t.align 4\n");
    fprintf(f, "\t\t.global\t%s_CodeStart\n\t\t.global\t%s_CodeEnd\n", sym, sym);
    fprintf(f, "%s_CodeStart:\n", sym);
    if (mesh) {
        fprintf(f, "; a task program enters here; without one the program starts at the next instruction\n");
        snprintf(buf, sizeof(buf), "isw.x         VI15,%d(VI00)", PVK_VU1_MESH_STATE);
        vu(f, "NOP", buf, "where to return to");
    }
    if (task) {
        fprintf(f, "; the first work group, and the groups left along each axis\n");
        snprintf(buf, sizeof(buf), "lq            VF04,%d(VI00)", hdr_addr(PVK_VU1_HEADER_COUNTS));
        vu(f, "NOP", buf, NULL);
        snprintf(buf, sizeof(buf), "sq            VF04,%d(VI00)", PVK_VU1_TASK_LEFT);
        vu(f, "sub.xyzw      VF04,VF00,VF00", buf, NULL);
        snprintf(buf, sizeof(buf), "sq            VF04,%d(VI00)", PVK_VU1_TASK_NEXT);
        vu(f, "NOP", buf, NULL);
        fprintf(f, "%s_group:\n", sym);
    }
    fprintf(f, "; prologue: storage block addresses and the local size\n");
    for (int i = 0; i < sh->storageCount; i++) {
        snprintf(buf, sizeof(buf), "ilw.%c         VI%02d,%d(VI00)", "xyzw"[i], PVK_VU1_STORAGE_BASE_REG + i,
                 hdr_addr(PVK_VU1_HEADER_BASES));
        snprintf(buf2, sizeof(buf2), "storage '%s', first qword", sh->storage[i].name);
        vu(f, "NOP", buf, buf2);
    }
    vu(f, "sub.xyzw      VF03,VF00,VF00", "NOP", NULL);
    for (int i = 0; i < 3; i++) {
        float v = (float)sh->localSize[i];
        uint32_t bits;
        memcpy(&bits, &v, sizeof(bits));
        loi_text(bits, buf, sizeof(buf), val, sizeof(val));
        snprintf(buf2, sizeof(buf2), "local_size_%c = %u", "xyz"[i], (unsigned)sh->localSize[i]);
        vu(f, "NOP", buf, buf2);
        snprintf(buf, sizeof(buf), "addi.%c        VF03,VF03,I", "xyz"[i]);
        vu(f, buf, "NOP", NULL);
    }
    if (!task)
        vu(f, "sub.xyzw      VF01,VF00,VF00", "NOP", "gl_WorkGroupID = 0");
}

/* The loops over the work groups and over each group's invocations, outermost first; the
 * invocation's IDs go to their qwords before the body. A task program runs one group per pass. */
static void emit_compute_loops(FILE *f, const Shader *sh, const char *sym)
{
    char buf[64];

    if (sh->execModel == ExecTask) {
        fprintf(f, "; this work group, then invocations along z, y, x\n");
        snprintf(buf, sizeof(buf), "lq            VF01,%d(VI00)", PVK_VU1_TASK_NEXT);
        vu(f, "NOP", buf, "gl_WorkGroupID");
        snprintf(buf, sizeof(buf), "isw.w         VI00,%d(VI00)", PVK_VU1_MESH_STATE);
        vu(f, "NOP", buf, "no mesh tasks emitted yet");
    } else {
        fprintf(f, "; work groups along z, y, x, then invocations along z, y, x\n");
        snprintf(buf, sizeof(buf), "ilw.z         VI03,%d(VI00)", hdr_addr(PVK_VU1_HEADER_COUNTS));
        vu(f, "NOP", buf, "work groups along z");
        fprintf(f, "%s_gz:\n", sym);
        snprintf(buf, sizeof(buf), "ilw.y         VI02,%d(VI00)", hdr_addr(PVK_VU1_HEADER_COUNTS));
        vu(f, "sub.y         VF01,VF00,VF00", buf, "work groups along y");
        fprintf(f, "%s_gy:\n", sym);
        snprintf(buf, sizeof(buf), "ilw.x         VI01,%d(VI00)", hdr_addr(PVK_VU1_HEADER_COUNTS));
        vu(f, "sub.x         VF01,VF00,VF00", buf, "work groups along x");
        fprintf(f, "%s_gx:\n", sym);
    }
    if (sh->execModel == ExecMesh) {
        snprintf(buf, sizeof(buf), "isw.x         VI00,%d(VI00)", PVK_VU1_MESH_OUTPUTS);
        vu(f, "NOP", buf, "no vertices until SetMeshOutputsEXT");
        snprintf(buf, sizeof(buf), "isw.y         VI00,%d(VI00)", PVK_VU1_MESH_OUTPUTS);
        vu(f, "NOP", buf, "no primitives");
    }
    if (sh->barriers) {
        vu(f, "NOP", "iaddiu        VI07,VI00,0", "barrier(): the first pass over the group's invocations");
        fprintf(f, "%s_pass:\n", sym);
        snprintf(buf, sizeof(buf), "iaddiu        VI14,VI00,%d", ctx_start(sh));
        vu(f, "NOP", buf, "where the first invocation keeps its registers");
        vu(f, "NOP", "iaddiu        VI15,VI00,0", "no invocation waits at a barrier()");
    }
    snprintf(buf, sizeof(buf), "iaddiu        VI06,VI00,%u", (unsigned)sh->localSize[2]);
    vu(f, "sub.xyzw      VF02,VF00,VF00", buf, "local ID and index 0; invocations along z");
    fprintf(f, "%s_lz:\n", sym);
    snprintf(buf, sizeof(buf), "iaddiu        VI05,VI00,%u", (unsigned)sh->localSize[1]);
    vu(f, "sub.y         VF02,VF00,VF00", buf, "invocations along y");
    fprintf(f, "%s_ly:\n", sym);
    snprintf(buf, sizeof(buf), "iaddiu        VI04,VI00,%u", (unsigned)sh->localSize[0]);
    vu(f, "sub.x         VF02,VF00,VF00", buf, "invocations along x");
    fprintf(f, "%s_lx:\n", sym);
    snprintf(buf, sizeof(buf), "sq.xyz        VF01,%d(VI00)", hdr_addr(PVK_VU1_HEADER_WORKGROUP));
    vu(f, "mul.xyz       VF04,VF01,VF03", buf, "gl_WorkGroupID * local size");
    snprintf(buf, sizeof(buf), "sq            VF02,%d(VI00)", hdr_addr(PVK_VU1_HEADER_LOCAL));
    vu(f, "add.xyz       VF04,VF04,VF02", buf, "+ gl_LocalInvocationID");
    snprintf(buf, sizeof(buf), "sq.xyz        VF04,%d(VI00)", hdr_addr(PVK_VU1_HEADER_GLOBAL));
    vu(f, "NOP", buf, "gl_GlobalInvocationID");
    if (sh->barriers) {
        snprintf(buf, sizeof(buf), "ibeq          VI07,VI00,%s_run", sym);
        vu(f, "NOP", buf, "first pass: the invocation starts");
        snprintf(buf, sizeof(buf), "ilw.x         VI13,%d(VI14)", ctx_off(sh, 0));
        vu(f, "NOP", buf, "delay slot: where the invocation waits, 0 = it has returned");
        vu(f, "NOP", "NOP", "the branch reads VI after one instruction");
        snprintf(buf, sizeof(buf), "ibeq          VI13,VI00,%s_done", sym);
        vu(f, "NOP", buf, NULL);
        vu(f, "NOP", "NOP", "branch delay slot");
        vu(f, "NOP", "jr            VI13", "go on behind its barrier()");
        vu(f, "NOP", "NOP", "branch delay slot");
        fprintf(f, "%s_run:\n", sym);
    }
}

/* One loop's end: count the axis up, count the loop down, branch back while it has iterations left. */
static void emit_loop_end(FILE *f, const char *sym, const char *upper, int vi, const char *label, const char *what)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "isubiu        VI%02d,VI%02d,1", vi, vi);
    vu(f, upper, buf, what);
    vu(f, "NOP", "NOP", "the branch reads VI after one instruction");
    snprintf(buf, sizeof(buf), "ibne          VI%02d,VI00,%s_%s", vi, sym, label);
    vu(f, "NOP", buf, NULL);
    vu(f, "NOP", "NOP", "branch delay slot");
}

static void emit_local_ends(FILE *f, const Shader *sh, const char *sym)
{
    fprintf(f, "; next invocation%s\n", sh->execModel == ExecGLCompute ? ", then next work group" : "");
    emit_loop_end(f, sym, "addw.xw       VF02,VF02,VF00w", 4, "lx", "local x + 1, index + 1");
    emit_loop_end(f, sym, "addw.y        VF02,VF02,VF00w", 5, "ly", "local y + 1");
    emit_loop_end(f, sym, "addw.z        VF02,VF02,VF00w", 6, "lz", "local z + 1");
}

static void emit_group_ends(FILE *f, const Shader *sh, const char *sym)
{
    if (sh->execModel != ExecGLCompute)
        fprintf(f, "; next work group\n");
    emit_loop_end(f, sym, "addw.x        VF01,VF01,VF00w", 1, "gx", "work group x + 1");
    emit_loop_end(f, sym, "addw.y        VF01,VF01,VF00w", 2, "gy", "work group y + 1");
    emit_loop_end(f, sym, "addw.z        VF01,VF01,VF00w", 3, "gz", "work group z + 1");
}

/* A lower instruction alone, or under the given upper one. */
static void lo(FILE *f, const char *upper, const char *comment, const char *fmt, ...)
{
    char buf[96];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    vu(f, upper ? upper : "NOP", buf, comment);
}

/* A mesh work group's geometry: vertex records to GS formats in place, then each primitive's
 * vertices into a GIF packet, kicked when full while the next fills the other packet. */
static void emit_mesh_geometry(FILE *f, const Shader *sh, const char *sym)
{
    static const int addr[3] = { 8, 13, 15 };
    int st = sh->texcoordVar ? 1 : 0, fog = sh->fogVar ? 1 : 0, out = 2 + st + fog, vpp = sh->meshPrimVertices;
    int vertexColor = sh->colorVar && !sh->meshColorPerPrim;

    fprintf(f, "; geometry: the vertex records to screen coordinates and GS formats, in place\n");
    lo(f, NULL, "primitives written", "ilw.y         VI04,%d(VI00)", PVK_VU1_MESH_OUTPUTS);
    lo(f, NULL, "vertices written", "ilw.x         VI14,%d(VI00)", PVK_VU1_MESH_OUTPUTS);
    lo(f, NULL, "viewport scale", "lq            VF26,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_VIEWPORT_SCALE);
    lo(f, NULL, "viewport offset", "lq            VF27,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_VIEWPORT_OFFSET);
    lo(f, NULL, "color scale", "lq            VF28,%d(VI00)", PVK_VU1_CONTEXT_ADDR + PVK_VU1_CTX_COLOR_SCALE);
    lo(f, NULL, "no primitives: nothing to draw", "ibeq          VI04,VI00,%s_drawn", sym);
    lo(f, NULL, "delay slot: first vertex record", "iaddiu        VI05,VI00,%d", sh->vertBase);
    lo(f, NULL, "no vertices", "ibeq          VI14,VI00,%s_drawn", sym);
    if (st)
        lo(f, NULL, "delay slot: 1 = UV in texels (PRIM.FST)", "ilw.y         VI13,%d(VI00)", PVK_VU1_MESH_STATE);
    else
        vu(f, "NOP", "NOP", "branch delay slot");
    fprintf(f, "%s_vertex:\n", sym);
    lo(f, NULL, "gl_Position", "lq            VF24,0(VI05)");
    vu(f, "NOP", "div           Q,VF00w,VF24w", "Q = 1 / w");
    vu(f, "NOP", "waitq", NULL);
    vu(f, "mulq.xyz      VF25,VF24,Q", "NOP", NULL);
    if (st) {
        lo(f, NULL, "texture coordinate", "lq            VF29,%d(VI05)", sh->texcoordSlot);
        lo(f, NULL, NULL, "ibne          VI13,VI00,%s_uv", sym);
        vu(f, "NOP", "NOP", "branch delay slot");
        vu(f, "mulq.xy       VF29,VF29,Q", "NOP", "S / w, T / w");
        lo(f, NULL, NULL, "b             %s_st", sym);
        vu(f, "addq.z        VF29,VF00,Q", "NOP", "delay slot: Q = 1 / w");
        fprintf(f, "%s_uv:\n", sym);
        vu(f, "ftoi4.xy      VF29,VF29", "NOP", "U, V to 12.4 fixed point");
        fprintf(f, "%s_st:\n", sym);
        lo(f, NULL, "ST (and Q), or UV", "sq            VF29,%d(VI05)", sh->texcoordSlot);
    }
    vu(f, "mul.xyz       VF25,VF25,VF26", "NOP", "viewport scale");
    vu(f, "add.xyz       VF25,VF25,VF27", "NOP", "viewport offset");
    lo(f, NULL, "screen x, y and depth, floats for the winding test", "sq.xyz        VF25,0(VI05)");
    if (vertexColor) {
        lo(f, NULL, "color", "lq            VF30,%d(VI05)", sh->colorSlot);
        vu(f, "mul.xyzw      VF30,VF30,VF28", "NOP", "color 0..1 to GS range");
        vu(f, "ftoi0.xyzw    VF30,VF30", "NOP", NULL);
        lo(f, NULL, "RGBAQ", "sq            VF30,%d(VI05)", sh->colorSlot);
    }
    if (fog) {
        lo(f, "sub.w         VF04,VF00,VF00", "fog amount", "lq            VF31,%d(VI05)", sh->fogSlot);
        vu(f, "maxx.w        VF04,VF04,VF31x", "NOP", "not below 0");
        vu(f, "miniw.w       VF04,VF04,VF00w", "NOP", "not above 1");
        vu(f, "mulx.w        VF04,VF04,VF28x", "NOP", "0..1 to 0..255");
        vu(f, "ftoi4.w       VF04,VF04", "NOP", "the FOG register's F field starts at bit 4 of w");
        lo(f, NULL, "FOG", "sq.w          VF04,%d(VI05)", sh->fogSlot);
    }
    vu(f, "NOP", "isubiu        VI14,VI14,1", NULL);
    lo(f, NULL, "next vertex record", "iaddiu        VI05,VI05,%d", sh->vertQwords);
    lo(f, NULL, NULL, "ibne          VI14,VI00,%s_vertex", sym);
    vu(f, "NOP", "NOP", "branch delay slot");

    if (sh->meshColorPerPrim) {
        fprintf(f, "; the primitives' colors to the GS range\n");
        lo(f, NULL, "first primitive record", "iaddiu        VI05,VI00,%d", sh->primBase);
        vu(f, "NOP", "iadd          VI14,VI04,VI00", "primitives");
        fprintf(f, "%s_color:\n", sym);
        lo(f, NULL, "color", "lq            VF30,%d(VI05)", sh->colorSlot);
        vu(f, "mul.xyzw      VF30,VF30,VF28", "isubiu        VI14,VI14,1", "color 0..1 to GS range");
        vu(f, "ftoi0.xyzw    VF30,VF30", "NOP", NULL);
        lo(f, NULL, "RGBAQ", "sq            VF30,%d(VI05)", sh->colorSlot);
        lo(f, NULL, NULL, "ibne          VI14,VI00,%s_color", sym);
        lo(f, NULL, "delay slot: next primitive record", "iaddiu        VI05,VI05,%d", sh->primQwords);
    } else if (!sh->colorVar) {
        vu(f, "mul.xyzw      VF30,VF00,VF28", "NOP", "no color output: black");
        vu(f, "ftoi0.xyzw    VF30,VF30", "NOP", NULL);
    }

    fprintf(f, "; each primitive's vertices into the GIF packet\n");
    lo(f, NULL, "first primitive record", "iaddiu        VI05,VI00,%d", sh->primBase);
    lo(f, NULL, "the GIF packet to fill", "ilw.w         VI06,%d(VI00)", PVK_VU1_MESH_OUTPUTS);
    lo(f, "sub.w         VF31,VF00,VF00", "z = winding sign; XYZ2 w = 0 (ADC off)", "lq            VF29,%d(VI00)",
       PVK_VU1_MESH_CULL);
    vu(f, "NOP", "iaddiu        VI14,VI00,0", "vertices in the packet");
    vu(f, "NOP", "iaddiu        VI06,VI06,1", "past its tag");
    fprintf(f, "%s_prim:\n", sym);
    if (sh->meshCull)
        vu(f, "NOP", "ilw.w         VI07,0(VI05)", "gl_CullPrimitiveEXT");
    for (int k = 0; k < vpp; k++)
        lo(f, NULL, k ? NULL : "the primitive's vertex indices", "ilw.%c         VI%02d,0(VI05)", "xyz"[k], addr[k]);
    if (sh->meshCull) {
        lo(f, NULL, "culled by the shader", "ibne          VI07,VI00,%s_skip", sym);
        vu(f, "NOP", "NOP", "branch delay slot");
    }
    for (int k = 0; k < vpp && sh->vertQwords > 1; k++) {
        /* index * qwords per vertex record */
        if (sh->vertQwords == 3) {
            lo(f, NULL, k ? NULL : "index * 3 qwords", "iadd          VI07,VI%02d,VI%02d", addr[k], addr[k]);
            lo(f, NULL, NULL, "iadd          VI%02d,VI07,VI%02d", addr[k], addr[k]);
        } else {
            lo(f, NULL, k ? NULL : "index * qwords per record", "iadd          VI%02d,VI%02d,VI%02d", addr[k],
               addr[k], addr[k]);
            if (sh->vertQwords == 4)
                lo(f, NULL, NULL, "iadd          VI%02d,VI%02d,VI%02d", addr[k], addr[k], addr[k]);
        }
    }
    for (int k = 0; k < vpp; k++)
        lo(f, NULL, k ? NULL : "screen positions", "lq            VF%02d,%d(VI%02d)", 24 + k, sh->vertBase, addr[k]);
    if (vpp == 3) {
        fprintf(f, "; culling: a negative signed area * winding sign drops the triangle\n");
        lo(f, "sub.xyz       VF27,VF25,VF24", "P1 - P0; 1 = cull by winding", "ilw.z         VI07,%d(VI00)",
           PVK_VU1_MESH_STATE);
        vu(f, "sub.xyz       VF28,VF26,VF24", "NOP", "P2 - P0");
        lo(f, NULL, "no culling", "ibeq          VI07,VI00,%s_draw", sym);
        vu(f, "NOP", "NOP", "branch delay slot");
        vu(f, "opmula.xyz    ACC,VF27,VF28", "NOP", NULL);
        vu(f, "opmsub.xyz    VF27,VF28,VF27", "NOP", "z = signed area");
        vu(f, "mul.z         VF27,VF27,VF29", "NOP", "area * sign, negative = cull");
        vu(f, "NOP", "iaddiu        VI07,VI00,0x20", "MAC flags land 4 cycles later; z negative flag");
        vu(f, "NOP", "NOP", NULL);
        vu(f, "NOP", "NOP", NULL);
        vu(f, "NOP", "NOP", NULL);
        vu(f, "NOP", "fmand         VI07,VI07", "sign flag of z");
        vu(f, "NOP", "NOP", "the branch reads VI after one instruction");
        lo(f, NULL, "back facing", "ibne          VI07,VI00,%s_skip", sym);
        vu(f, "NOP", "NOP", "branch delay slot");
        fprintf(f, "%s_draw:\n", sym);
    }
    for (int k = 0; k < vpp; k++) {
        char up[48];

        snprintf(up, sizeof(up), "ftoi4.xy      VF31,VF%02d", 24 + k);
        if (st)
            lo(f, up, "x, y to 12.4 fixed point; ST or UV", "lq            VF04,%d(VI%02d)",
               sh->vertBase + sh->texcoordSlot, addr[k]);
        else
            vu(f, up, "NOP", "x, y to 12.4 fixed point");
        snprintf(up, sizeof(up), "ftoi0.z       VF31,VF%02d", 24 + k);
        if (st)
            lo(f, up, "z to integer depth", "sq            VF04,%d(VI06)", k * out);
        else
            vu(f, up, "NOP", "z to integer depth");
        if (vertexColor)
            lo(f, NULL, "RGBAQ", "lq            VF04,%d(VI%02d)", sh->vertBase + sh->colorSlot, addr[k]);
        else if (sh->meshColorPerPrim)
            lo(f, NULL, "RGBAQ of the primitive", "lq            VF04,%d(VI05)", sh->colorSlot);
        lo(f, NULL, NULL, "sq            VF%02d,%d(VI06)", sh->colorVar ? 4 : 30, k * out + st);
        if (fog) {
            lo(f, NULL, "FOG", "lq            VF04,%d(VI%02d)", sh->vertBase + sh->fogSlot, addr[k]);
            lo(f, NULL, NULL, "sq            VF04,%d(VI06)", k * out + st + 1);
        }
        lo(f, NULL, "XYZ2", "sq            VF31,%d(VI06)", k * out + st + fog + 1);
    }
    lo(f, NULL, NULL, "iaddiu        VI06,VI06,%d", vpp * out);
    lo(f, NULL, NULL, "iaddiu        VI14,VI14,%d", vpp);
    fprintf(f, "%s_skip:\n", sym);
    lo(f, NULL, "next primitive record", "iaddiu        VI05,VI05,%d", sh->primQwords);
    vu(f, "NOP", "isubiu        VI04,VI04,1", NULL);
    lo(f, NULL, "vertices of a full packet", "iaddiu        VI07,VI00,%d", sh->kickVertices);
    vu(f, "NOP", "NOP", "the branch reads VI after one instruction");
    lo(f, NULL, "full: send it", "ibeq          VI14,VI07,%s_kick", sym);
    vu(f, "NOP", "NOP", "branch delay slot");
    lo(f, NULL, NULL, "ibne          VI04,VI00,%s_prim", sym);
    vu(f, "NOP", "NOP", "branch delay slot");
    lo(f, NULL, "the last packet is empty", "ibeq          VI14,VI00,%s_drawn", sym);
    vu(f, "NOP", "NOP", "branch delay slot");
    fprintf(f, "%s_kick:\n", sym);
    fprintf(f, "; send the packet to the GS; the next one fills the other packet meanwhile\n");
    lo(f, NULL, NULL, "ilw.w         VI07,%d(VI00)", PVK_VU1_MESH_OUTPUTS);
    lo(f, NULL, "GIF tag", "lq            VF04,%d(VI00)", PVK_VU1_MESH_GIFTAG);
    vu(f, "NOP", "iaddiu        VI08,VI14,0x4000", NULL);
    vu(f, "NOP", "iaddiu        VI08,VI08,0x4000", "EOP");
    vu(f, "NOP", "sq            VF04,0(VI07)", NULL);
    vu(f, "NOP", "isw.x         VI08,0(VI07)", "NLOOP = its vertices");
    vu(f, "NOP", "xgkick        VI07", NULL);
    lo(f, NULL, NULL, "iaddiu        VI08,VI00,%d", sh->kickA + sh->kickB);
    vu(f, "NOP", "isub          VI07,VI08,VI07", "the other packet");
    lo(f, NULL, NULL, "isw.w         VI07,%d(VI00)", PVK_VU1_MESH_OUTPUTS);
    vu(f, "NOP", "iaddiu        VI06,VI07,1", "past its tag");
    vu(f, "NOP", "iaddiu        VI14,VI00,0", NULL);
    lo(f, NULL, NULL, "ibne          VI04,VI00,%s_prim", sym);
    vu(f, "NOP", "NOP", "branch delay slot");
    fprintf(f, "%s_drawn:\n", sym);
}

/* Compute stops after its last group; mesh returns to its caller or stops; task steps to its
 * next group, calls the mesh program behind it if the group emitted tasks, and starts over. */
static void emit_compute_tail(FILE *f, const Shader *sh, const char *sym)
{
    char buf[64];

    if (sh->barriers) {
        snprintf(buf, sizeof(buf), "b             %s_done", sym);
        vu(f, "NOP", buf, "the invocation has returned");
        snprintf(buf, sizeof(buf), "isw.x         VI00,%d(VI14)", ctx_off(sh, 0));
        vu(f, "NOP", buf, "delay slot: nowhere to go on");
        fprintf(f, "%s_wait:\n", sym);
        snprintf(buf, sizeof(buf), "isw.x         VI13,%d(VI14)", ctx_off(sh, 0));
        vu(f, "NOP", buf, "barrier(): where the invocation goes on");
        vu(f, "NOP", "iaddiu        VI15,VI00,1", "an invocation waits");
        fprintf(f, "%s_done:\n", sym);
        snprintf(buf, sizeof(buf), "%s        VI14,VI14,%d", sh->execModel == ExecMesh ? "isubiu" : "iaddiu",
                 g_ctxQwords);
        vu(f, "NOP", buf, "the next invocation's registers");
    }
    emit_local_ends(f, sh, sym);
    if (sh->barriers) {
        snprintf(buf, sizeof(buf), "ibeq          VI15,VI00,%s_passed", sym);
        vu(f, "NOP", buf, "no invocation waits: the group is done");
        vu(f, "NOP", "NOP", "branch delay slot");
        snprintf(buf, sizeof(buf), "b             %s_pass", sym);
        vu(f, "NOP", buf, "another pass");
        vu(f, "NOP", "iaddiu        VI07,VI00,1", "delay slot: invocations go on where they wait");
        fprintf(f, "%s_passed:\n", sym);
    }
    if (sh->execModel == ExecTask) {
        fprintf(f, "; the next work group: x, then y, then z\n");
        for (int i = 0; i < 3; i++) {
            snprintf(buf, sizeof(buf), "ilw.%c         VI%02d,%d(VI00)", "xyz"[i], 1 + i, PVK_VU1_TASK_LEFT);
            vu(f, "NOP", buf, i ? NULL : "work groups left along x, y, z");
        }
        for (int i = 0; i < 3; i++) {
            char up[48];

            snprintf(up, sizeof(up), "addw.%c        VF01,VF01,VF00w", "xyz"[i]);
            snprintf(buf, sizeof(buf), "isubiu        VI%02d,VI%02d,1", 1 + i, 1 + i);
            vu(f, up, buf, NULL);
            if (i == 2)
                break;
            vu(f, "NOP", "NOP", "the branch reads VI after one instruction");
            snprintf(buf, sizeof(buf), "ibne          VI%02d,VI00,%s_next", 1 + i, sym);
            vu(f, "NOP", buf, NULL);
            vu(f, "NOP", "NOP", "branch delay slot");
            snprintf(up, sizeof(up), "sub.%c         VF01,VF00,VF00", "xyz"[i]);
            snprintf(buf, sizeof(buf), "ilw.%c         VI%02d,%d(VI00)", "xyz"[i], 1 + i, hdr_addr(PVK_VU1_HEADER_COUNTS));
            vu(f, up, buf, "this axis starts over");
        }
        fprintf(f, "%s_next:\n", sym);
        snprintf(buf, sizeof(buf), "sq.xyz        VF01,%d(VI00)", PVK_VU1_TASK_NEXT);
        vu(f, "NOP", buf, NULL);
        for (int i = 0; i < 3; i++) {
            snprintf(buf, sizeof(buf), "isw.%c         VI%02d,%d(VI00)", "xyz"[i], 1 + i, PVK_VU1_TASK_LEFT);
            vu(f, "NOP", buf, NULL);
        }
        fprintf(f, "; the mesh program, if this group emitted mesh tasks\n");
        snprintf(buf, sizeof(buf), "ilw.w         VI08,%d(VI00)", PVK_VU1_MESH_STATE);
        vu(f, "NOP", buf, NULL);
        vu(f, "NOP", "NOP", "the branch reads VI after one instruction");
        snprintf(buf, sizeof(buf), "ibeq          VI08,VI00,%s_meshed", sym);
        vu(f, "NOP", buf, NULL);
        vu(f, "NOP", "NOP", "branch delay slot");
        snprintf(buf, sizeof(buf), "bal           VI15,%s_CodeEnd", sym);
        vu(f, "NOP", buf, "the pipeline places the mesh program there");
        vu(f, "NOP", "NOP", "branch delay slot");
        fprintf(f, "%s_meshed:\n", sym);
        snprintf(buf, sizeof(buf), "ilw.z         VI03,%d(VI00)", PVK_VU1_TASK_LEFT);
        vu(f, "NOP", buf, "0 after the last work group");
        vu(f, "NOP", "NOP", "the branch reads VI after one instruction");
        snprintf(buf, sizeof(buf), "ibne          VI03,VI00,%s_group", sym);
        vu(f, "NOP", buf, NULL);
        vu(f, "NOP", "NOP", "branch delay slot");
    } else if (sh->execModel == ExecMesh) {
        emit_mesh_geometry(f, sh, sym);
        emit_group_ends(f, sh, sym);
        fprintf(f, "; back to the task program, if one called\n");
        snprintf(buf, sizeof(buf), "ilw.x         VI15,%d(VI00)", PVK_VU1_MESH_STATE);
        vu(f, "NOP", buf, NULL);
        vu(f, "NOP", "NOP", "the branch reads VI after one instruction");
        snprintf(buf, sizeof(buf), "ibeq          VI15,VI00,%s_stop", sym);
        vu(f, "NOP", buf, NULL);
        vu(f, "NOP", "NOP", "branch delay slot");
        vu(f, "NOP", "jr            VI15", NULL);
        vu(f, "NOP", "NOP", "branch delay slot");
        fprintf(f, "%s_stop:\n", sym);
    } else {
        emit_group_ends(f, sh, sym);
    }
    fprintf(f, "; stop\n");
    vu(f, "NOP[E]", "NOP", NULL);
    vu(f, "NOP", "NOP", NULL);
}

static void emit_vertex(const Shader *sh, const IdInfo *ids, const char *base, const char *sym)
{
    FILE *f = open_out(base, ".vsm");
    char buf[64], buf2[64];
    static Regs r;
    static int def[MAX_IDS], labelAt[MAX_IDS], targeted[MAX_IDS];
    int i, constants = 0, compute = IS_GROUPED(sh), mesh = sh->execModel == ExecMesh;

    g_sym = sym;
    g_lazyConst = mesh;
    g_pool = compute ? g_computePool : g_vertexPool;
    g_poolCount = compute ? (int)(sizeof(g_computePool) / sizeof(g_computePool[0]))
                          : (int)(sizeof(g_vertexPool) / sizeof(g_vertexPool[0]));

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
    if (sh->fogValue)
        r.lastUse[canon(ids, sh, sh->fogValue)] = MAX_BODY;
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

    if (!compute) {
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
    } else {
        emit_compute_head(f, sh, sym);
    }

    for (i = 0; i <= sh->bodyCount + 3 && !g_lazyConst; i++) {
        uint32_t use[MAX_OPERANDS] = { 0 };
        if (i < sh->bodyCount) {
            for (int j = 0; j < sh->body[i].nops && !sh->body[i].dead; j++)
                use[j] = sh->body[i].ops[j];
        } else if (i == sh->bodyCount) {
            use[0] = sh->positionValue;
        } else if (i == sh->bodyCount + 1) {
            use[0] = sh->colorValue;
        } else if (i == sh->bodyCount + 2) {
            use[0] = sh->texcoordValue;
        } else {
            use[0] = sh->fogValue;
        }
        for (int j = 0; j < MAX_OPERANDS; j++) {
            uint32_t c = use[j] ? root(ids, use[j]) : 0, k;
            if (!c || ids[c].kind != ID_CONST || r.reg[c] || ids[c].vf00)
                continue;
            /* a constant with the same value already has a register; a specialization
             * constant keeps its own, since the pipeline may change its value */
            for (k = is_spec(&ids[c]) ? sh->constBase : 0; k < sh->constBase; k++)
                if (k != c && ids[k].kind == ID_CONST && r.reg[k] && !is_spec(&ids[k]) &&
                    ids[k].compCount == ids[c].compCount &&
                    !memcmp(ids[k].comps, ids[c].comps, sizeof(uint32_t) * (size_t)ids[c].compCount))
                    break;
            if (k < sh->constBase) {
                r.reg[c] = r.reg[k];
                continue;
            }
            if (!constants++)
                fprintf(f, "; constants\n");
            r.reg[c] = reg_alloc(&r);
            emit_constant(f, ids, &ids[c], r.reg[c], sym);
        }
    }

    if (!compute) {
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
    } else {
        emit_compute_loops(f, sh, sym);
    }

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
        if (b->op == OpBarrier) {
            emit_barrier(f, &r, ids, sh, line);
            continue;
        }
        if (b->op == OpStorStore || b->op == OpEmitTasks) {
            int t;
            for (int j = 0; j < b->nops; j++)
                reg_ensure_loaded(f, &r, ids, sh, b->ops[j]);
            for (int j = 0; j < b->nops; j++)
                regs[j] = reg_of(&r, ids, sh, b->ops[j]);
            t = reg_alloc(&r);
            if (b->op == OpEmitTasks)
                emit_tasks(f, ids, b, t, regs, line);
            else
                emit_stor_store(f, ids, b, t, regs, line);
            r.busy[t] = 0;
            for (int j = 0; j < b->nops; j++)
                reg_release(&r, ids, sh, b->ops[j], i);
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
    if (!compute) {
        reg_ensure_loaded(f, &r, ids, sh, sh->positionValue);
        if (sh->colorValue)
            reg_ensure_loaded(f, &r, ids, sh, sh->colorValue);
        if (sh->texcoordValue)
            reg_ensure_loaded(f, &r, ids, sh, sh->texcoordValue);
        if (sh->fogValue)
            reg_ensure_loaded(f, &r, ids, sh, sh->fogValue);
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
        if (sh->fogValue) {
            /* The fog amount rides in VF22's w field, which the position leaves free. */
            char c = "xyzw"[ids[sh->fogValue].comp];
            vu(f, "sub.w         VF22,VF00,VF00", "NOP", NULL);
            snprintf(buf, sizeof(buf), "max%c.w        VF22,VF22,VF%02d%c", c, reg_of(&r, ids, sh, sh->fogValue), c);
            snprintf(buf2, sizeof(buf2), "fog output, location %d, not below 0", sh->fogLocation);
            vu(f, buf, "NOP", buf2);
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
        if (sh->fogValue) {
            vu(f, "miniw.w       VF22,VF22,VF00w", "NOP", "fog not above 1");
            vu(f, "mulx.w        VF22,VF22,VF03x", "NOP", "fog 0..1 to 0..255");
            vu(f, "ftoi4.w       VF22,VF22", "NOP", "the FOG register's F field starts at bit 4 of w");
        }
        {
            /* [ST,] RGBAQ, [FOG,] XYZ2 */
            int st = sh->texcoordValue ? 1 : 0, fog = sh->fogValue ? 1 : 0;

            if (st) {
                snprintf(buf, sizeof(buf), "sq            VF%02d,0(VI05)", PVK_VU1_TEXCOORD_REG);
                vu(f, "NOP", buf, "ST (and Q), or UV");
            }
            snprintf(buf, sizeof(buf), "sq            VF24,%d(VI05)", st);
            vu(f, "NOP", buf, "RGBAQ");
            if (fog) {
                snprintf(buf, sizeof(buf), "sq            VF22,%d(VI05)", st + 1);
                vu(f, "NOP", buf, "FOG");
            }
            emit_cull(f, sym);
            snprintf(buf, sizeof(buf), "sq            VF23,%d(VI05)", st + fog + 1);
            vu(f, "NOP", buf, "XYZ2 (draws unless ADC is set)");
            snprintf(buf, sizeof(buf), "iaddiu        VI05,VI05,%d", PVK_VU1_OUTPUT_PER_VERTEX + st + fog);
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
    } else {
        emit_compute_tail(f, sh, sym);
    }
    vu(f, "NOP", "NOP", NULL);
    fprintf(f, "\t\t.align 4\n%s_CodeEnd:\n", sym);
    for (i = 0; i < g_clamps; i++)
        fprintf(f, "\t\t.global\t%s_Clamp%d\n", sym, i);
    for (i = 0; i < g_specCount; i++)
        fprintf(f, "\t\t.global\t%s_Spec%d\n", sym, i);
    fclose(f);

    {
        PvkPvsHeader h;
        PvkPvsSpecConstant specs[MAX_SPECS];
        uint32_t *clamps = calloc((size_t)g_clamps + 1, sizeof(uint32_t));
        char obj[1100], cmd[2400], suffix[32];
        uint32_t start, end;
        VuObject o;

        if (!clamps)
            fail("out of memory");
        /* dvp-as turns the .vsm into microcode; the labels give each offset in it. */
        g_line = 0;
        snprintf(obj, sizeof(obj), "%s_vu1.o", base);
        snprintf(cmd, sizeof(cmd), "dvp-as \"%s.vsm\" -o \"%s\"", base, obj);
        if (system(cmd) != 0)
            fail("dvp-as failed on %s.vsm (the ps2dev toolchain must be on PATH)", base);
        read_vu_object(obj, &o);
        start = vu_label(obj, &o, sym, "_CodeStart");
        end   = vu_label(obj, &o, sym, "_CodeEnd");
        if (end <= start)
            fail("%s holds no microcode", obj);

        memset(&h, 0, sizeof(h));
        h.stage       = mesh ? PVK_SHADER_STAGE_MESH_BIT_EXT : sh->execModel == ExecTask ? PVK_SHADER_STAGE_TASK_BIT_EXT
                      : compute ? PVK_SHADER_STAGE_COMPUTE_BIT : PVK_SHADER_STAGE_VERTEX;
        h.codeWords   = (end - start) / 4;
        h.convertWord = compute ? 0 : (vu_label(obj, &o, sym, "_Convert") - start) / 4;
        h.clampCount  = (uint32_t)g_clamps;
        for (i = 0; i < g_clamps; i++) {
            snprintf(suffix, sizeof(suffix), "_Clamp%d", i);
            clamps[i] = (vu_label(obj, &o, sym, suffix) - start) / 4;
        }
        h.specCount = (uint32_t)g_specCount;
        for (i = 0; i < g_specCount; i++) {
            snprintf(suffix, sizeof(suffix), "_Spec%d", i);
            specs[i].constantID = (uint32_t)g_specs[i].id;
            specs[i].word       = (vu_label(obj, &o, sym, suffix) - start) / 4;
            specs[i].type       = (uint32_t)g_specs[i].type;
        }
        h.inputCount = (uint32_t)sh->inputCount;
        for (i = 0; i < sh->inputCount; i++)
            h.inputLocations[i] = (uint32_t)sh->inputs[i].location;
        h.colorOutputLocation    = (mesh ? sh->colorVar : sh->colorValue) ? sh->colorLocation : -1;
        h.texcoordOutputLocation = (mesh ? sh->texcoordVar : sh->texcoordValue) ? sh->texcoordLocation : -1;
        h.fogOutputLocation      = (mesh ? sh->fogVar : sh->fogValue) ? sh->fogLocation : -1;
        h.pushConstantBytes      = sh->pushBytes;
        h.uniformBlockCount      = (uint32_t)sh->uniformCount;
        for (i = 0; i < sh->uniformCount; i++) {
            h.uniformBlocks[i].set     = (uint32_t)sh->uniforms[i].set;
            h.uniformBlocks[i].binding = (uint32_t)sh->uniforms[i].binding;
            h.uniformBlocks[i].qword   = sh->uniforms[i].qword;
            h.uniformBlocks[i].bytes   = sh->uniforms[i].bytes;
        }
        h.colorInputLocation = h.texcoordInputLocation = h.fogInputLocation = -1;
        h.samplerBinding = h.samplerSet = -1;
        for (i = 0; compute && i < 3; i++)
            h.localSize[i] = sh->localSize[i];
        h.storageBlockCount = compute ? (uint32_t)sh->storageCount : 0;
        for (i = 0; compute && i < sh->storageCount; i++) {
            h.storageBlocks[i].set         = (uint32_t)sh->storage[i].set;
            h.storageBlocks[i].binding     = (uint32_t)sh->storage[i].binding;
            h.storageBlocks[i].bytes       = sh->storage[i].bytes;
            h.storageBlocks[i].arrayOffset = sh->storage[i].arrayOffset;
            h.storageBlocks[i].arrayStride = sh->storage[i].arrayStride;
            h.storageBlocks[i].written     = (uint32_t)sh->storage[i].written;
        }
        if (mesh) {
            h.meshMaxVertices       = sh->meshMaxVertices;
            h.meshMaxPrimitives     = sh->meshMaxPrimitives;
            h.meshPrimitiveVertices = (uint32_t)sh->meshPrimVertices;
            h.meshOutputAddress     = (uint32_t)sh->vertBase;
            h.meshKickAddress       = (uint32_t)sh->kickA;
        }
        h.payloadBytes = (uint32_t)sh->payloadQwords * 16;
        if (compute) {
            /* Behind the body, where the registers a barrier() keeps are known. */
            int kept = g_ctxQwords * (int)(sh->localSize[0] * sh->localSize[1] * sh->localSize[2]);
            int top = mesh ? sh->vertBase : PVK_VU1_MEMORY_QWORDS;

            if (PVK_VU1_PAYLOAD_ADDR + sh->payloadQwords + sh->sharedQwords + kept > top)
                fail("compute shader's barrier() keeps %d qwords for each of its %u invocations, %d bytes of "
                     "VU1 memory; behind the task payload and the shared variables it has %d", g_ctxQwords,
                     (unsigned)(sh->localSize[0] * sh->localSize[1] * sh->localSize[2]), kept * 16,
                     (top - PVK_VU1_PAYLOAD_ADDR - sh->payloadQwords - sh->sharedQwords) * 16);
            h.sharedBytes = (uint32_t)(sh->sharedQwords + kept) * 16;
        }
        write_pvs(base, &h, g_src, sh->entryPoint, (const uint32_t *)(o.text + start), clamps, specs);
        remove(obj);
        free((void *)o.data);
        free(clamps);
    }
}

static void emit_fragment(const Shader *sh, const IdInfo *ids, const char *base)
{
    int texel = sh->colorValue ? ids[sh->colorValue].texel : TEXEL_NONE;
    int usesColor = sh->colorValue && (texel == TEXEL_NONE || texel == TEXEL_SAMPLE_TIMES_COLOR);
    int usesTexture = texel != TEXEL_NONE;
    int atst = 0, aref = 0;
    PvkPvsHeader h;

    if (sh->discard)
        frag_alpha_test(sh, ids, &atst, &aref);

    memset(&h, 0, sizeof(h));
    h.stage = PVK_SHADER_STAGE_FRAGMENT;
    h.colorOutputLocation = h.texcoordOutputLocation = h.fogOutputLocation = -1;
    h.colorInputLocation    = usesColor && sh->colorInput >= 0 ? sh->inputs[sh->colorInput].location : -1;
    h.texcoordInputLocation = usesTexture ? sh->inputs[sh->texcoordInput].location : -1;
    h.gouraud               = usesColor ? 1 : 0;           /* GS PRIM.IIP */
    h.textured              = usesTexture ? 1 : 0;         /* GS PRIM.TME */
    h.textureFunction       = usesTexture ? (uint32_t)sh->textureFunction : 0; /* 0 modulate, 1 decal */
    h.samplerBinding        = usesTexture ? sh->samplerBinding : -1;
    h.samplerSet            = usesTexture ? sh->samplerSet : -1;
    h.alphaTest             = sh->discard ? 1 : 0;
    h.alphaTestFunction     = (uint32_t)atst;
    h.alphaTestReference    = (uint32_t)aref;
    h.fogInputLocation      = sh->fogged ? sh->inputs[sh->fogInput].location : -1;
    h.fog                   = sh->fogged ? 1 : 0;
    h.fogColor              = sh->fogged ? sh->fogColor : 0;
    write_pvs(base, &h, g_src, sh->entryPoint, NULL, NULL, NULL);
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

/* A .pvs file read back, checked far enough for shaderc's own use. */
static PvkPvsHeader *read_pvs(const char *path, size_t *size)
{
    PvkPvsHeader *h = (PvkPvsHeader *)read_file(path, size);

    if (*size < sizeof(*h) || h->magic != PVK_PVS_MAGIC || h->version != PVK_PVS_VERSION ||
        h->fileBytes != *size || h->nameOffset >= *size || h->entryPointOffset >= *size ||
        h->clampsOffset + (size_t)h->clampCount * 4 > *size ||
        h->specsOffset + (size_t)h->specCount * sizeof(PvkPvsSpecConstant) > *size ||
        h->codeOffset + (size_t)h->codeWords * 4 > *size)
        fail("%s is not a .pvs file of this shaderc", path);
    return h;
}

/* shaderc --listing: <base>.txt, the .pvs file read back as text and, for a vertex
 * shader, the VU1 assembly its microcode was assembled from. */
static void write_listing(const char *base)
{
    char path[1100];
    size_t size;
    const PvkPvsHeader *h;
    const uint8_t *file;
    const PvkPvsSpecConstant *specs;
    const uint32_t *clamps;
    FILE *f, *vsm;
    int c;

    snprintf(path, sizeof(path), "%s.pvs", base);
    h      = read_pvs(path, &size);
    file   = (const uint8_t *)h;
    specs  = (const PvkPvsSpecConstant *)(file + h->specsOffset);
    clamps = (const uint32_t *)(file + h->clampsOffset);
    f      = open_out(base, ".txt");

    fprintf(f, "name = %s\n", (const char *)file + h->nameOffset);
    fprintf(f, "entryPoint = %s\n", (const char *)file + h->entryPointOffset);
    if (h->stage != PVK_SHADER_STAGE_FRAGMENT) {
        int grouped = h->stage != PVK_SHADER_STAGE_VERTEX;

        fprintf(f, "stage = %s\n", h->stage == PVK_SHADER_STAGE_VERTEX ? "vertex"
                : h->stage == PVK_SHADER_STAGE_COMPUTE_BIT ? "compute"
                : h->stage == PVK_SHADER_STAGE_TASK_BIT_EXT ? "task" : "mesh");
        fprintf(f, "codeWords = %u\n", (unsigned)h->codeWords);
        fprintf(f, "convertWord = %u\n", (unsigned)h->convertWord);
        fprintf(f, "clamps =");
        for (uint32_t i = 0; i < h->clampCount; i++)
            fprintf(f, " %u", (unsigned)clamps[i]);
        fprintf(f, "\n");
        for (uint32_t i = 0; i < h->specCount; i++)
            fprintf(f, "specConstant = { constantID %u, word %u, %s }\n", (unsigned)specs[i].constantID,
                   (unsigned)specs[i].word, specs[i].type < 4 ? spec_type_names[specs[i].type] : "?");
        if (grouped) {
            fprintf(f, "localSize = %u %u %u\n", (unsigned)h->localSize[0], (unsigned)h->localSize[1],
                    (unsigned)h->localSize[2]);
            for (uint32_t i = 0; i < h->storageBlockCount && i < PVK_MAX_STORAGE_BLOCKS; i++)
                fprintf(f, "storageBlock = { set %u, binding %u, bytes %u, arrayOffset %u, arrayStride %u, written %u }\n",
                        (unsigned)h->storageBlocks[i].set, (unsigned)h->storageBlocks[i].binding,
                        (unsigned)h->storageBlocks[i].bytes, (unsigned)h->storageBlocks[i].arrayOffset,
                        (unsigned)h->storageBlocks[i].arrayStride, (unsigned)h->storageBlocks[i].written);
        }
        if (h->stage == PVK_SHADER_STAGE_MESH_BIT_EXT) {
            fprintf(f, "meshMaxVertices = %u\n", (unsigned)h->meshMaxVertices);
            fprintf(f, "meshMaxPrimitives = %u\n", (unsigned)h->meshMaxPrimitives);
            fprintf(f, "meshPrimitiveVertices = %u\n", (unsigned)h->meshPrimitiveVertices);
            fprintf(f, "meshOutputAddress = %u\n", (unsigned)h->meshOutputAddress);
            fprintf(f, "meshKickAddress = %u\n", (unsigned)h->meshKickAddress);
        }
        if (h->stage == PVK_SHADER_STAGE_MESH_BIT_EXT || h->stage == PVK_SHADER_STAGE_TASK_BIT_EXT)
            fprintf(f, "payloadBytes = %u\n", (unsigned)h->payloadBytes);
        if (h->stage == PVK_SHADER_STAGE_MESH_BIT_EXT || h->stage == PVK_SHADER_STAGE_TASK_BIT_EXT)
            fprintf(f, "sharedBytes = %u\n", (unsigned)h->sharedBytes);
        fprintf(f, "inputLocations =");
        for (uint32_t i = 0; i < h->inputCount && i < PVK_MAX_VERTEX_INPUTS; i++)
            fprintf(f, " %u", (unsigned)h->inputLocations[i]);
        fprintf(f, "\n");
        fprintf(f, "colorOutputLocation = %d\n", (int)h->colorOutputLocation);
        fprintf(f, "texcoordOutputLocation = %d\n", (int)h->texcoordOutputLocation);
        fprintf(f, "fogOutputLocation = %d\n", (int)h->fogOutputLocation);
        fprintf(f, "pushConstantBytes = %u\n", (unsigned)h->pushConstantBytes);
        for (uint32_t i = 0; i < h->uniformBlockCount && i < PVK_MAX_UNIFORM_BLOCKS; i++)
            fprintf(f, "uniformBlock = { set %u, binding %u, qword %u, bytes %u }\n",
                   (unsigned)h->uniformBlocks[i].set, (unsigned)h->uniformBlocks[i].binding,
                   (unsigned)h->uniformBlocks[i].qword, (unsigned)h->uniformBlocks[i].bytes);
    } else {
        fprintf(f, "stage = fragment\n");
        fprintf(f, "colorInputLocation = %d\n", (int)h->colorInputLocation);
        fprintf(f, "texcoordInputLocation = %d\n", (int)h->texcoordInputLocation);
        fprintf(f, "gouraud = %u\n", (unsigned)h->gouraud);
        fprintf(f, "textured = %u\n", (unsigned)h->textured);
        fprintf(f, "textureFunction = %u\n", (unsigned)h->textureFunction);
        fprintf(f, "samplerBinding = %d\n", (int)h->samplerBinding);
        fprintf(f, "samplerSet = %d\n", (int)h->samplerSet);
        fprintf(f, "alphaTest = %u\n", (unsigned)h->alphaTest);
        fprintf(f, "alphaTestFunction = %u\n", (unsigned)h->alphaTestFunction);
        fprintf(f, "alphaTestReference = %u\n", (unsigned)h->alphaTestReference);
        fprintf(f, "fogInputLocation = %d\n", (int)h->fogInputLocation);
        fprintf(f, "fog = %u\n", (unsigned)h->fog);
        fprintf(f, "fogColor = 0x%06x\n", (unsigned)h->fogColor);
    }
    snprintf(path, sizeof(path), "%s.vsm", base);
    vsm = fopen(path, "r");
    if (vsm) {
        fputc('\n', f);
        while ((c = fgetc(vsm)) != EOF)
            fputc(c, f);
        fclose(vsm);
    }
    if (ferror(f) || fclose(f))
        fail("cannot write %s.txt", base);
    free((void *)h);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *outBase = NULL;
    static IdInfo ids[MAX_IDS];
    uint32_t *code;
    size_t words;
    Shader sh;
    char sym[256], vsm[1100];
    int i, listing = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            outBase = argv[++i];
        else if (!strcmp(argv[i], "--listing"))
            listing = 1;
        else
            in = argv[i];
    }
    if (!in || !outBase) {
        fprintf(stderr, "usage: shaderc [--listing] input.spv -o output_base\n");
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

    if (IS_CODE(&sh))
        emit_vertex(&sh, ids, outBase, sym);
    else
        emit_fragment(&sh, ids, outBase);

    if (listing)
        write_listing(outBase);
    snprintf(vsm, sizeof(vsm), "%s.vsm", outBase);
    remove(vsm);

    free(code);
    return 0;
}
