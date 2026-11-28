/* Tessellation of a patch's parameter space: from the levels a control shader wrote, the list of
 * gl_TessCoord values the evaluation program runs over, 3 per triangle, 2 per line, 1 per point. */
#include <math.h>

#include "pvk_internal.h"

/* How one edge is cut: n segments, of which a fractional spacing makes the first and last shorter. */
struct Cut {
    int   n;
    float f;       /* the clamped level */
    int   spacing; /* 0 equal, 1 fractional even, 2 fractional odd */
};

struct Out {
    float   *v;        /* u, v, w, 0 per vertex, or NULL to count */
    uint32_t n;
    int      triangle; /* barycentric: winding is told in the (v, w) plane; else in (u, v) */
    float    want;     /* sign of the area a triangle is given: + for cw, - for ccw */
};

static struct Cut cut_of(float level, int spacing)
{
    float lo = spacing == 1 ? 2.0f : 1.0f, hi = spacing == 2 ? PVK_MAX_TESSELLATION_LEVEL - 1 : PVK_MAX_TESSELLATION_LEVEL;
    struct Cut c;

    c.f = level < lo ? lo : level > hi ? hi : level;
    c.n = (int)ceilf(c.f);
    if (spacing == 1 && (c.n & 1))
        c.n++;
    if (spacing == 2 && !(c.n & 1))
        c.n++;
    c.spacing = spacing;
    return c;
}

/* Where cut j of the edge lies, 0 to 1. Fractional: n - 2 segments of 1 / f and a shorter one at each end. */
static float cut_at(const struct Cut *c, int j)
{
    if (j <= 0)
        return 0.0f;
    if (j >= c->n)
        return 1.0f;
    if (c->spacing == 0)
        return (float)j / (float)c->n;
    return (1.0f - (float)(c->n - 2) / c->f) * 0.5f + (float)(j - 1) / c->f;
}

static void put(struct Out *o, const float *p)
{
    if (o->v) {
        o->v[o->n * 4 + 0] = p[0];
        o->v[o->n * 4 + 1] = p[1];
        o->v[o->n * 4 + 2] = p[2];
        o->v[o->n * 4 + 3] = 0.0f;
    }
    o->n++;
}

/* A triangle in the winding the evaluation shader's layout asks for; one without area is left out. */
static void tri(struct Out *o, const float *a, const float *b, const float *c)
{
    int x = o->triangle ? 1 : 0, y = x + 1;
    float area = (b[x] - a[x]) * (c[y] - a[y]) - (b[y] - a[y]) * (c[x] - a[x]);

    if (area == 0.0f)
        return;
    put(o, a);
    put(o, area * o->want > 0.0f ? b : c);
    put(o, area * o->want > 0.0f ? c : b);
}

static void lerp(float *out, const float *a, const float *b, float t)
{
    out[0] = a[0] + (b[0] - a[0]) * t;
    out[1] = a[1] + (b[1] - a[1]) * t;
    out[2] = a[2] + (b[2] - a[2]) * t;
}

#define MAX_POINTS (PVK_MAX_TESSELLATION_LEVEL + 1)

/* A row of points and where each lies along the strip it borders, 0 to 1. */
struct Row {
    float p[MAX_POINTS][3];
    float t[MAX_POINTS];
    int   segments;
};

/* The triangles between two rows: whichever row's next point comes first makes the next one. */
static void stitch(struct Out *o, const struct Row *a, const struct Row *b)
{
    int i = 0, j = 0;

    while (i < a->segments || j < b->segments) {
        if (j == b->segments || (i < a->segments && a->t[i + 1] <= b->t[j + 1])) {
            tri(o, a->p[i], a->p[i + 1], b->p[j]);
            i++;
        } else {
            tri(o, a->p[i], b->p[j + 1], b->p[j]);
            j++;
        }
    }
}

/* Corner `which` (0 = u, 1 = v, 2 = w) of the triangle ring whose corners weigh `c` on the others. */
static void ring_corner(float *out, int which, float c)
{
    out[0] = out[1] = out[2] = c;
    out[which] = 1.0f - 2.0f * c;
}

/* Edge e of a triangle runs between the corners that are not e: outer level e cuts the edge u, v or w = 0. */
static const int edge_from[3] = { 1, 2, 0 }, edge_to[3] = { 2, 0, 1 };

/* Edge e of ring k: the inner level's cuts k to n - k, as they project onto the ring. */
static void ring_row(struct Row *r, const struct Cut *in, int k, int e, float t0, float t1)
{
    float c = cut_at(in, k) * (2.0f / 3.0f), a[3], b[3], lo = cut_at(in, k), hi = cut_at(in, in->n - k);
    int m = in->n - 2 * k, j;

    ring_corner(a, edge_from[e], c);
    ring_corner(b, edge_to[e], c);
    r->segments = m > 0 ? m : 0;
    for (j = 0; j <= r->segments; j++) {
        float s = m > 0 ? (cut_at(in, k + j) - lo) / (hi - lo) : 0.0f;

        lerp(r->p[j], a, b, s);
        r->t[j] = m > 0 ? (cut_at(in, k + j) - t0) / (t1 - t0) : 0.5f;
    }
}

static void triangles(struct Out *o, const struct Cut *outer, struct Cut in, int points)
{
    static struct Row a, b;
    int k, e, j;

    if (outer[0].n == 1 && outer[1].n == 1 && outer[2].n == 1 && in.n == 1) {
        float c[3][3];
        for (e = 0; e < 3; e++)
            ring_corner(c[e], e, 0.0f);
        if (points)
            for (e = 0; e < 3; e++)
                put(o, c[e]);
        else
            tri(o, c[0], c[1], c[2]);
        return;
    }
    /* An inner level of 1 beside a cut edge counts as just above 1. */
    if (in.n == 1)
        in = cut_of(1.0f + 1.0f / 1024.0f, in.spacing);

    for (k = 0; in.n - 2 * k >= 0; k++) {
        int m = in.n - 2 * k;

        if (m == 0 || (m == 1 && !points)) {
            /* The middle: one point, or the last triangle. */
            float c[3][3];
            for (e = 0; e < 3; e++)
                ring_corner(c[e], e, cut_at(&in, k) * (2.0f / 3.0f));
            if (m == 0 && points)
                put(o, c[0]);
            else if (m == 1)
                tri(o, c[0], c[1], c[2]);
            break;
        }
        for (e = 0; e < 3; e++) {
            if (k == 0) {
                /* The patch's own edge, cut by its outer level. */
                float from[3], to[3];
                ring_corner(from, edge_from[e], 0.0f);
                ring_corner(to, edge_to[e], 0.0f);
                a.segments = outer[e].n;
                for (j = 0; j <= a.segments; j++) {
                    a.t[j] = cut_at(&outer[e], j);
                    lerp(a.p[j], from, to, a.t[j]);
                }
            } else {
                ring_row(&a, &in, k, e, cut_at(&in, k), cut_at(&in, in.n - k));
            }
            if (points) {
                for (j = 0; j < a.segments; j++)
                    put(o, a.p[j]);
                continue;
            }
            ring_row(&b, &in, k + 1, e, cut_at(&in, k), cut_at(&in, in.n - k));
            stitch(o, &a, &b);
        }
    }
}

static void quads(struct Out *o, const struct Cut *outer, struct Cut iu, struct Cut iv, int points)
{
    static struct Row a, b;
    static const float corner[4][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } };
    /* Edge e runs from corner[from] to corner[to]: u = 0, v = 0, u = 1, v = 1. */
    static const int from[4] = { 0, 0, 1, 3 }, to[4] = { 3, 1, 2, 2 };
    int e, i, j;

    if (outer[0].n == 1 && outer[1].n == 1 && outer[2].n == 1 && outer[3].n == 1 && iu.n == 1 && iv.n == 1) {
        if (points) {
            for (e = 0; e < 4; e++)
                put(o, corner[e]);
        } else {
            tri(o, corner[0], corner[1], corner[2]);
            tri(o, corner[0], corner[2], corner[3]);
        }
        return;
    }
    if (iu.n == 1)
        iu = cut_of(1.0f + 1.0f / 1024.0f, iu.spacing);
    if (iv.n == 1)
        iv = cut_of(1.0f + 1.0f / 1024.0f, iv.spacing);

    /* The inside: the cells between the inner levels' cuts 1 to n - 1. */
    for (j = 1; j < iv.n; j++) {
        for (i = 1; i < iu.n; i++) {
            float p[4][3] = { { cut_at(&iu, i), cut_at(&iv, j), 0 }, { cut_at(&iu, i + 1), cut_at(&iv, j), 0 },
                              { cut_at(&iu, i + 1), cut_at(&iv, j + 1), 0 }, { cut_at(&iu, i), cut_at(&iv, j + 1), 0 } };
            if (points) {
                put(o, p[0]);
            } else if (i < iu.n - 1 && j < iv.n - 1) {
                tri(o, p[0], p[1], p[2]);
                tri(o, p[0], p[2], p[3]);
            }
        }
    }
    /* The border: each edge, cut by its outer level, against the side of the inside it faces. */
    for (e = 0; e < 4; e++) {
        int alongV = e == 0 || e == 2;
        const struct Cut *in = alongV ? &iv : &iu;
        float fixed = e == 0 ? cut_at(&iu, 1) : e == 2 ? cut_at(&iu, iu.n - 1)
                    : e == 1 ? cut_at(&iv, 1) : cut_at(&iv, iv.n - 1);

        a.segments = outer[e].n;
        for (j = 0; j <= a.segments; j++) {
            a.t[j] = cut_at(&outer[e], j);
            lerp(a.p[j], corner[from[e]], corner[to[e]], a.t[j]);
        }
        if (points) {
            /* Edges 0 and 1 both start at (0, 0): the second leaves it out. */
            for (j = e == 1; j < a.segments; j++)
                put(o, a.p[j]);
            continue;
        }
        b.segments = in->n - 2;
        for (j = 0; j <= b.segments; j++) {
            b.t[j] = cut_at(in, j + 1);
            b.p[j][0] = alongV ? fixed : b.t[j];
            b.p[j][1] = alongV ? b.t[j] : fixed;
            b.p[j][2] = 0.0f;
        }
        stitch(o, &a, &b);
    }
    /* The far corner (1, 1) ends no edge's row. */
    if (points)
        put(o, corner[2]);
}

static void isolines(struct Out *o, const struct Cut *lines, const struct Cut *cut, int points)
{
    int l, s;

    for (l = 0; l < lines->n; l++) {
        for (s = 0; s < cut->n + (points ? 1 : 0); s++) {
            float p[2][3] = { { cut_at(cut, s), (float)l / (float)lines->n, 0 },
                              { cut_at(cut, s + 1), (float)l / (float)lines->n, 0 } };
            put(o, p[0]);
            if (!points)
                put(o, p[1]);
        }
    }
}

uint32_t pvk_tessellate(uint32_t domain, uint32_t spacing, int clockwise, int pointMode, const float *outer,
                        const float *inner, float *out)
{
    struct Out o = { out, 0, domain == 0, clockwise ? 1.0f : -1.0f };
    uint32_t used = domain == 0 ? 3 : domain == 1 ? 4 : 2, i;
    struct Cut c[4];

    /* A patch with an outer level of 0 or less, or not a number, is dropped. */
    for (i = 0; i < used; i++)
        if (!(outer[i] > 0.0f))
            return 0;
    for (i = 0; i < used; i++)
        c[i] = cut_of(outer[i], (int)spacing);
    if (domain == 0) {
        triangles(&o, c, cut_of(inner[0], (int)spacing), pointMode);
    } else if (domain == 1) {
        quads(&o, c, cut_of(inner[0], (int)spacing), cut_of(inner[1], (int)spacing), pointMode);
    } else {
        c[0] = cut_of(outer[0], 0); /* the number of lines is always a whole one */
        isolines(&o, &c[0], &c[1], pointMode);
    }
    return o.n;
}
