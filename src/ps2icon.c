/*
 * PS2 save icon library - see ps2icon.h for the format and coordinate system.
 */

#include <tamtypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ps2icon.h"
#include "gs_render.h"
#include "sjis.h"
#include "ui.h"

/* icon.sys field offsets. */
#define SYS_LINE2 6
#define SYS_BG_COLOR 16
#define SYS_LIGHT_DIR 80
#define SYS_LIGHT_COL 128
#define SYS_AMBIENT 176
#define SYS_TITLE 192
#define SYS_TITLE_LEN 68
#define SYS_ICON_NORMAL 260
#define SYS_ICON_NAME_LEN 64
/* The copy and delete names follow the normal one back to back. */
#define SYS_ICON_STRIDE 64

/* .ico header. */
#define ICO_MAGIC 0x00010000
#define ICO_HDR_SIZE 20
#define ICO_TEX_PRESENT 0x04
#define ICO_TEX_COMPRESSED 0x08

/* Scratch buffers for a draw or rasterize call. */
static float scr_x[PS2ICON_MAX_VERTS];
static float scr_y[PS2ICON_MAX_VERTS];
static float scr_z[PS2ICON_MAX_VERTS];
static u32 scr_col[PS2ICON_MAX_VERTS];
static int tri_order[PS2ICON_MAX_VERTS / 3];
static float tri_depth[PS2ICON_MAX_VERTS / 3];

static u32 rd_u32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static s16 rd_s16(const u8 *p) { return (s16)(p[0] | (p[1] << 8)); }

static float rd_f32(const u8 *p)
{
    union { u32 u; float f; } v;
    v.u = rd_u32(p);
    return v.f;
}

/* ---------------------------------------------------------------- icon.sys */

/* Converts a Shift-JIS title to ASCII. Single-byte values pass through and
   double-byte ones go through sjis_wide_char. A space is emitted at `line2`,
   the byte offset where the title's second line starts. */
static void sjis_to_ascii(const u8 *src, int src_len, int line2, char *dst, int dst_len)
{
    int o = 0;

    for (int i = 0; i < src_len && o < dst_len - 1; ) {
        if (i == line2 && o > 0) {
            dst[o++] = ' ';
            if (o >= dst_len - 1) {
                break;
            }
        }

        u8 c = src[i];
        if (c == 0) {
            break;
        }
        if (c < 0x80) {
            dst[o++] = (c >= 0x20) ? (char)c : ' ';
            i++;
            continue;
        }
        if (i + 1 >= src_len) {
            break;
        }

        dst[o++] = sjis_wide_char((u16)((c << 8) | src[i + 1]));
        i += 2;
    }

    while (o > 0 && dst[o - 1] == ' ') {
        o--;
    }
    dst[o] = '\0';
}

int ps2icon_sys_parse(const void *buf, int len, ps2icon_sys *out)
{
    const u8 *sys = (const u8 *)buf;

    if (len < PS2ICON_SYS_SIZE || memcmp(sys, "PS2D", 4) != 0) {
        return 0;
    }

    sjis_to_ascii(&sys[SYS_TITLE], SYS_TITLE_LEN, rd_u16(&sys[SYS_LINE2]),
                  out->title, PS2ICON_TITLE_MAX);

    for (int s = 0; s < PS2ICON_STATE_COUNT; s++) {
        memcpy(out->icon_file[s], &sys[SYS_ICON_NORMAL + s * SYS_ICON_STRIDE],
               SYS_ICON_NAME_LEN);
        out->icon_file[s][SYS_ICON_NAME_LEN] = '\0';
    }

    /* Backdrop corners: four channels of u32, each 0..255. */
    for (int i = 0; i < 4; i++) {
        u32 r = rd_u32(&sys[SYS_BG_COLOR + i * 16]);
        u32 g = rd_u32(&sys[SYS_BG_COLOR + i * 16 + 4]);
        u32 b = rd_u32(&sys[SYS_BG_COLOR + i * 16 + 8]);
        out->bg_color[i] = UI_RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
    }

    /* Lights: direction and colour as four floats each, ambient likewise. */
    for (int i = 0; i < 3; i++) {
        for (int c = 0; c < 3; c++) {
            out->light_dir[i][c] = rd_f32(&sys[SYS_LIGHT_DIR + i * 16 + c * 4]);
            out->light_col[i][c] = rd_f32(&sys[SYS_LIGHT_COL + i * 16 + c * 4]);
        }

        float d = out->light_dir[i][0] * out->light_dir[i][0] +
                  out->light_dir[i][1] * out->light_dir[i][1] +
                  out->light_dir[i][2] * out->light_dir[i][2];
        if (d > 0.0001f) {
            float inv = 1.0f / sqrtf(d);
            out->light_dir[i][0] *= inv;
            out->light_dir[i][1] *= inv;
            out->light_dir[i][2] *= inv;
        }
    }
    for (int c = 0; c < 3; c++) {
        out->ambient[c] = rd_f32(&sys[SYS_AMBIENT + c * 4]);
    }

    return 1;
}

const char *ps2icon_sys_icon(const ps2icon_sys *sys, ps2icon_state state)
{
    if (state < 0 || state >= PS2ICON_STATE_COUNT ||
        sys->icon_file[state][0] == '\0') {
        state = PS2ICON_STATE_NORMAL;
    }
    return sys->icon_file[state];
}

/* Lighting for one vertex normal: ambient plus the diffuse term of each
   light. The light vectors point FROM the surface TOWARDS the light source
   negated, so the dot product uses -dir. Lights are not rotated with the
   model, which keeps a spinning icon's shading attached to the model exactly
   as the PS2 browser does.

   The result is the multiplier applied to the texture, packed as a GS vertex
   colour where 128 means 1.0 (the GS modulates as texel * colour / 128), so
   values above 1.0 brighten and 255 caps at 2x. */
static u32 shade(const ps2icon_sys *sys, float nx, float ny, float nz)
{
    float len = nx * nx + ny * ny + nz * nz;
    if (len > 0.0001f) {
        float inv = 1.0f / sqrtf(len);
        nx *= inv; ny *= inv; nz *= inv;
    }

    float rgb[3];
    for (int c = 0; c < 3; c++) {
        rgb[c] = sys->ambient[c];
    }

    for (int i = 0; i < 3; i++) {
        float d = -(nx * sys->light_dir[i][0] +
                    ny * sys->light_dir[i][1] +
                    nz * sys->light_dir[i][2]);
        if (d < 0.0f) {
            d = 0.0f;
        }
        for (int c = 0; c < 3; c++) {
            rgb[c] += sys->light_col[i][c] * d;
        }
    }

    int out[3];
    for (int c = 0; c < 3; c++) {
        out[c] = (int)(rgb[c] * 128.0f);
        if (out[c] < 0) { out[c] = 0; }
        if (out[c] > 255) { out[c] = 255; }
    }

    return UI_RGB(out[0], out[1], out[2]);
}

/* -------------------------------------------------------------------- .ico */

/* Decodes the 128x128 A1B5G5R5 texture at `off` into m->tex, forcing every
   texel opaque (icon textures do not use the alpha bit). */
static void decode_texture(const u8 *ico, int len, u32 off, int compressed,
                           ps2icon_model *m)
{
    memset(m->tex, 0, sizeof(m->tex));

    if (!compressed) {
        for (int i = 0; i < PS2ICON_TEX_PIXELS && off + 2 <= (u32)len; i++, off += 2) {
            m->tex[i] = rd_u16(&ico[off]);
        }
    } else {
        /* u32 byte count, then a stream of u16 RLE codes. A code below 0xFF00
           is a repeat count for the single u16 that follows; 0xFF00 or above
           is a literal run of (0x10000 - code) values, i.e. 1 to 256. (The
           2003 format document gives 0xFFFF - code here, which is off by one
           and desynchronises everything after the first literal run.) */
        u32 packed = rd_u32(&ico[off]);
        off += 4;
        u32 end = off + packed;
        if (end > (u32)len) {
            end = (u32)len;
        }

        int out = 0;
        while (off + 2 <= end && out < PS2ICON_TEX_PIXELS) {
            u16 code = rd_u16(&ico[off]);
            off += 2;

            if (code < 0xFF00) {
                if (off + 2 > end) {
                    break;
                }
                u16 val = rd_u16(&ico[off]);
                off += 2;
                for (int r = 0; r < code && out < PS2ICON_TEX_PIXELS; r++) {
                    m->tex[out++] = val;
                }
            } else {
                int n = 0x10000 - code;
                for (int r = 0; r < n && out < PS2ICON_TEX_PIXELS && off + 2 <= end;
                     r++, off += 2) {
                    m->tex[out++] = rd_u16(&ico[off]);
                }
            }
        }
    }

    for (int i = 0; i < PS2ICON_TEX_PIXELS; i++) {
        m->tex[i] |= 0x8000;
    }
}

int ps2icon_parse(const void *buf, int len, ps2icon_model *out)
{
    const u8 *ico = (const u8 *)buf;

    if (len < ICO_HDR_SIZE || rd_u32(&ico[0]) != ICO_MAGIC) {
        return 0;
    }

    u32 file_shapes = rd_u32(&ico[4]);
    u32 tex_type = rd_u32(&ico[8]);
    u32 verts = rd_u32(&ico[16]);

    if (file_shapes < 1 || file_shapes > PS2ICON_MAX_SHAPES ||
        verts < 3 || verts > PS2ICON_MAX_VERTS) {
        return 0;
    }

    u32 shapes = file_shapes;
    u32 stride = file_shapes * 8 + 16;
    u32 off = ICO_HDR_SIZE;
    if (off + (u64)verts * stride > (u32)len) {
        return 0;
    }

    for (u32 i = 0; i < verts; i++) {
        const u8 *vp = &ico[off + i * stride];

        for (u32 s = 0; s < shapes; s++) {
            const u8 *sp = vp + s * 8;
            out->pos[s][i][0] = rd_s16(sp);
            out->pos[s][i][1] = rd_s16(sp + 2);
            out->pos[s][i][2] = rd_s16(sp + 4);
        }

        const u8 *np = vp + file_shapes * 8;
        out->nrm[i][0] = rd_s16(np);
        out->nrm[i][1] = rd_s16(np + 2);
        out->nrm[i][2] = rd_s16(np + 4);

        const u8 *uvp = np + 8;
        out->uv[i][0] = rd_s16(uvp);
        out->uv[i][1] = rd_s16(uvp + 2);

        const u8 *cp = uvp + 4;
        out->col[i][0] = cp[0];
        out->col[i][1] = cp[1];
        out->col[i][2] = cp[2];
        out->col[i][3] = cp[3];
    }

    /* Some icons address texture tiles past the first (UVs above 4096, i.e.
       beyond 1.0) and rely on the GS wrapping them. gsKit clamps UVs to the
       texture size before they reach the GS, so instead re-base each triangle
       into the first tile: whole-tile offsets shared by all three vertices
       are safe to subtract. Triangles that straddle a tile boundary keep
       their (rare) overflow and get edge-clamped. */
    for (u32 tri = 0; tri + 2 < verts; tri += 3) {
        for (int axis = 0; axis < 2; axis++) {
            s16 lo = out->uv[tri][axis];
            if (out->uv[tri + 1][axis] < lo) { lo = out->uv[tri + 1][axis]; }
            if (out->uv[tri + 2][axis] < lo) { lo = out->uv[tri + 2][axis]; }

            s16 tile = (s16)(lo / 4096);
            if (tile > 0) {
                out->uv[tri][axis] -= tile * 4096;
                out->uv[tri + 1][axis] -= tile * 4096;
                out->uv[tri + 2][axis] -= tile * 4096;
            }
        }
    }

    /* Animation segment: a 20-byte header, then an 8-byte record per frame
       (shape id, key count) followed by key_count 8-byte (time, value) keys.
       Each record is a weight track for the shape it names. This walk is what
       lands on the start of the texture segment, so a mis-sized record here
       shifts the whole texture; the layout was validated against real saves
       by checking that the walk ends exactly one texture before EOF. */
    u32 anim = off + verts * stride;
    if (anim + ICO_HDR_SIZE > (u32)len) {
        return 0;
    }

    u32 frame_length = rd_u32(&ico[anim + 4]);
    float anim_speed = rd_f32(&ico[anim + 8]);
    u32 frames = rd_u32(&ico[anim + 16]);

    out->frame_length = (frame_length > 0) ? (float)frame_length : 1.0f;
    out->anim_speed = (anim_speed > 0.001f && anim_speed < 100.0f) ? anim_speed : 1.0f;

    out->frames = 0;
    u32 total_keys = 0;
    u32 pos = anim + 20;
    for (u32 f = 0; f < frames && pos + 8 <= (u32)len; f++) {
        u32 sid = rd_u32(&ico[pos]);
        u32 keys = rd_u32(&ico[pos + 4]);

        if (out->frames < PS2ICON_MAX_FRAMES) {
            u32 idx = out->frames++;
            u32 stored = 0;

            out->frame_shape[idx] = (u8)(sid < shapes ? sid : 0);
            out->frame_key_start[idx] = (u16)total_keys;
            for (u32 k = 0; k < keys && total_keys < PS2ICON_MAX_KEYS &&
                 pos + 8 + k * 8 + 8 <= (u32)len; k++) {
                out->key_time[total_keys] = rd_f32(&ico[pos + 8 + k * 8]);
                out->key_value[total_keys] = rd_f32(&ico[pos + 8 + k * 8 + 4]);
                total_keys++;
                stored++;
            }
            out->frame_keys[idx] = (u8)(stored < 255 ? stored : 255);
        }
        pos += 8 + keys * 8;
    }

    if (!(tex_type & ICO_TEX_PRESENT) || pos >= (u32)len) {
        return 0;
    }
    decode_texture(ico, len, pos, (tex_type & ICO_TEX_COMPRESSED) != 0, out);

    out->shapes = shapes;
    out->verts = verts;
    return 1;
}

/* ------------------------------------------------------------------ shared */

/* The PS2 browser steps the animation timeline once per vsync. */
#define ANIM_TICK_HZ 60.0f

/* Computes every shape's blend weight for elapsed time `t`. The timeline
   position advances anim_speed ticks per 1/60s and wraps at frame_length;
   each frame record's key track is evaluated there (linear between keys,
   held flat outside them) and accumulated onto the shape it names. Weights
   are normalized so the blended pose never collapses or overshoots, and the
   key times are what set the playback direction - icons whose tracks run
   opposite to their storage order play backwards by design. */
static void shape_weights(const ps2icon_model *m, float t,
                          float w[PS2ICON_MAX_SHAPES])
{
    for (u32 s = 0; s < PS2ICON_MAX_SHAPES; s++) {
        w[s] = 0.0f;
    }

    if (m->shapes < 2 || m->frames < 2) {
        w[0] = 1.0f;
        return;
    }

    float c = fmodf(t * ANIM_TICK_HZ * m->anim_speed, m->frame_length);
    if (c < 0.0f) {
        c += m->frame_length;
    }

    float sum = 0.0f;
    for (u32 f = 0; f < m->frames; f++) {
        int n = m->frame_keys[f];
        if (n == 0) {
            continue;
        }

        const float *kt = &m->key_time[m->frame_key_start[f]];
        const float *kv = &m->key_value[m->frame_key_start[f]];
        float v;

        if (c <= kt[0]) {
            v = kv[0];
        } else if (c >= kt[n - 1]) {
            v = kv[n - 1];
        } else {
            v = kv[n - 1];
            for (int k = 0; k < n - 1; k++) {
                if (c <= kt[k + 1]) {
                    float span = kt[k + 1] - kt[k];
                    float fr = (span > 0.0001f) ? (c - kt[k]) / span : 0.0f;
                    v = kv[k] + (kv[k + 1] - kv[k]) * fr;
                    break;
                }
            }
        }

        if (v > 0.0f) {
            w[m->frame_shape[f]] += v;
            sum += v;
        }
    }

    if (sum < 0.0001f) {
        for (u32 s = 0; s < PS2ICON_MAX_SHAPES; s++) {
            w[s] = 0.0f;
        }
        w[0] = 1.0f;
        return;
    }
    for (u32 s = 0; s < m->shapes; s++) {
        w[s] /= sum;
    }
}

/* Bounding box of shape 0 in the XY plane, used to centre and scale. */
static void model_bounds(const ps2icon_model *m, float *cx, float *cy, float *extent)
{
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;

    for (u32 i = 0; i < m->verts; i++) {
        float x = m->pos[0][i][0] / PS2ICON_FIXED;
        float y = m->pos[0][i][1] / PS2ICON_FIXED;
        if (x < minx) { minx = x; }
        if (x > maxx) { maxx = x; }
        if (y < miny) { miny = y; }
        if (y > maxy) { maxy = y; }
    }

    *cx = (minx + maxx) * 0.5f;
    *cy = (miny + maxy) * 0.5f;
    float w = maxx - minx, h = maxy - miny;
    *extent = (w > h) ? w : h;
}

/* Projects every vertex into scr_x/scr_y/scr_z and lights it into scr_col. */
static void transform(const ps2icon_model *m, const ps2icon_sys *sys,
                      int cx, int cy, float scale, float t, float spin, int lights)
{
    float w[PS2ICON_MAX_SHAPES];
    shape_weights(m, t, w);

    /* Usually only two shapes carry weight at any instant; skip the rest. */
    u32 act[PS2ICON_MAX_SHAPES];
    float aw[PS2ICON_MAX_SHAPES];
    u32 na = 0;
    for (u32 s = 0; s < m->shapes; s++) {
        if (w[s] > 0.0001f) {
            act[na] = s;
            aw[na] = w[s];
            na++;
        }
    }
    if (na == 0) {
        act[0] = 0;
        aw[0] = 1.0f;
        na = 1;
    }

    float mcx, mcy, extent;
    model_bounds(m, &mcx, &mcy, &extent);

    float ca = cosf(spin), sa = sinf(spin);

    for (u32 i = 0; i < m->verts; i++) {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        for (u32 a = 0; a < na; a++) {
            x += aw[a] * m->pos[act[a]][i][0];
            y += aw[a] * m->pos[act[a]][i][1];
            z += aw[a] * m->pos[act[a]][i][2];
        }
        x /= PS2ICON_FIXED;
        y /= PS2ICON_FIXED;
        z /= PS2ICON_FIXED;

        x -= mcx;
        y -= mcy;

        scr_x[i] = cx + (x * ca + z * sa) * scale;
        scr_y[i] = cy + y * scale;
        scr_z[i] = -x * sa + z * ca;

        /* Untransformed normal: the lights rotate with the model, so the
           shading is fixed relative to the geometry. 128 is the GS's neutral
           modulation value, i.e. the texture passed through unshaded. */
        scr_col[i] = lights ? shade(sys, m->nrm[i][0] / PS2ICON_FIXED,
                                         m->nrm[i][1] / PS2ICON_FIXED,
                                         m->nrm[i][2] / PS2ICON_FIXED)
                            : UI_RGB(128, 128, 128);
    }
}

/* Sort-key offset that pushes a back face behind all front faces. Larger than
   any model's depth range, which is a couple of units at most. */
#define BACKFACE_BIAS 1000.0f

/* Orders the triangles far to near, back faces first. Returns how many. */
static int build_tri_list(const ps2icon_model *m);

static int tri_cmp(const void *a, const void *b)
{
    float da = tri_depth[*(const int *)a];
    float db = tri_depth[*(const int *)b];
    return (da > db) ? -1 : ((da < db) ? 1 : 0);
}

static int build_tri_list(const ps2icon_model *m)
{
    int kept = 0;
    int total = (int)(m->verts / 3);

    /* Back faces are drawn rather than culled, but pushed behind every front
       face. Icon meshes have inconsistent winding, so culling by facing tears
       holes in them, while drawing them in depth order alone lets them cover
       the front. Biasing their sort key keeps them underneath and lets them
       fill the gaps left by mis-wound triangles. */
    for (int i = 0; i < total; i++) {
        int a = i * 3, b = a + 1, c = a + 2;
        float area = (scr_x[b] - scr_x[a]) * (scr_y[c] - scr_y[a]) -
                     (scr_y[b] - scr_y[a]) * (scr_x[c] - scr_x[a]);

        if (area > -0.0001f && area < 0.0001f) {
            continue; /* degenerate */
        }

        tri_depth[i] = scr_z[a] + scr_z[b] + scr_z[c];
        if (area > 0.0f) {
            tri_depth[i] += BACKFACE_BIAS;
        }
        tri_order[kept++] = i;
    }

    qsort(tri_order, kept, sizeof(int), tri_cmp);
    return kept;
}

/* ------------------------------------------------------------- GS drawing */

void ps2icon_upload(const ps2icon_model *m, int slot)
{
    /* A slot reused from a PS1 save is still set to nearest sampling; these
       textures are detailed enough to want the filter back. */
    gs_icon_tex_filter(slot, 0);
    gs_icon_tex_upload(slot, m->tex);
}

void ps2icon_draw(const ps2icon_model *m, const ps2icon_sys *sys, int slot,
                  int cx, int cy, int size, float t, float spin, int lights)
{
    float mcx, mcy, extent;
    model_bounds(m, &mcx, &mcy, &extent);
    if (extent < 0.0001f) {
        return;
    }

    transform(m, sys, cx, cy, size / extent, t, spin, lights);
    int tris = build_tri_list(m);

    /* Per-pixel depth. Model Z grows away from the camera, GS depth grows
       towards it. */
    gs_depth_test(1);
    for (int k = 0; k < tris; k++) {
        int a = tri_order[k] * 3, b = a + 1, c = a + 2;

        gs_icon_tri(slot,
            scr_x[a], scr_y[a], GS_DEPTH_MID - (int)(scr_z[a] * GS_DEPTH_SCALE),
            m->uv[a][0] * PS2ICON_TEX_DIM / PS2ICON_FIXED,
            m->uv[a][1] * PS2ICON_TEX_DIM / PS2ICON_FIXED, scr_col[a],

            scr_x[b], scr_y[b], GS_DEPTH_MID - (int)(scr_z[b] * GS_DEPTH_SCALE),
            m->uv[b][0] * PS2ICON_TEX_DIM / PS2ICON_FIXED,
            m->uv[b][1] * PS2ICON_TEX_DIM / PS2ICON_FIXED, scr_col[b],

            scr_x[c], scr_y[c], GS_DEPTH_MID - (int)(scr_z[c] * GS_DEPTH_SCALE),
            m->uv[c][0] * PS2ICON_TEX_DIM / PS2ICON_FIXED,
            m->uv[c][1] * PS2ICON_TEX_DIM / PS2ICON_FIXED, scr_col[c]);
    }
    gs_depth_test(0);
}

void ps2icon_prepack(const ps2icon_model *m, const ps2icon_sys *sys, int slot,
                     int cx, int cy, int size)
{
    float mcx, mcy, extent;
    model_bounds(m, &mcx, &mcy, &extent);
    if (extent < 0.0001f) {
        return;
    }

    transform(m, sys, cx, cy, size / extent, 0.0f, 0.0f, 0);
    int tris = build_tri_list(m);

    gs_icon_static_begin(slot);
    for (int k = 0; k < tris; k++) {
        int a = tri_order[k] * 3, b = a + 1, c = a + 2;

        gs_icon_static_tri(
            scr_x[a], scr_y[a], GS_DEPTH_MID - (int)(scr_z[a] * GS_DEPTH_SCALE),
            m->uv[a][0] * PS2ICON_TEX_DIM / PS2ICON_FIXED,
            m->uv[a][1] * PS2ICON_TEX_DIM / PS2ICON_FIXED,

            scr_x[b], scr_y[b], GS_DEPTH_MID - (int)(scr_z[b] * GS_DEPTH_SCALE),
            m->uv[b][0] * PS2ICON_TEX_DIM / PS2ICON_FIXED,
            m->uv[b][1] * PS2ICON_TEX_DIM / PS2ICON_FIXED,

            scr_x[c], scr_y[c], GS_DEPTH_MID - (int)(scr_z[c] * GS_DEPTH_SCALE),
            m->uv[c][0] * PS2ICON_TEX_DIM / PS2ICON_FIXED,
            m->uv[c][1] * PS2ICON_TEX_DIM / PS2ICON_FIXED);
    }
    gs_icon_static_end();
}
