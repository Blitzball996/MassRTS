#pragma once
// ==========================================================================
// SDF Terrain - Plan B: the terrain IS a Signed Distance Field rendered with
// chunked Marching Cubes (a true volumetric body, not a height-field skin).
// The legacy heightmap is kept ONLY as a cheap query surface for units /
// camera / combat (get_height_at); it is no longer drawn.
//
// Memory: a large map at fine voxels would be gigabytes if every chunk stored
// a full float array. We use SPARSE chunks - a chunk allocates its SDF array
// only when the surface passes through it (or it gets carved). Chunks entirely
// solid or entirely air store a single uniform value and emit no mesh.
//
// Network: a carve is a deterministic event {center, radius, op, strength}.
// Each client applies the same carve -> identical SDF state (lockstep).
// ==========================================================================

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>
#include <glad/glad.h>

class Terrain;

extern const int MC_EDGE_TABLE[256];
extern const int MC_TRI_TABLE[256][16];

// ---- Tunables ----
static constexpr int   SDF_CHUNK_SIZE = 32;     // voxels per chunk axis
static constexpr float SDF_VOXEL_SIZE = 3.0f;   // world units per voxel
// Vertical volume must fully contain the tallest mountain peaks (~250 with
// height_scale presets) or peak chunks get wrongly flagged "all solid" and
// vanish (missing distant mountains + flat-topped solid-line artifacts). The
// ceiling is deliberately FAR above the dig floor: you can build several times
// higher than you can tunnel deep (asymmetric, per design).
static constexpr float SDF_WORLD_Y_MIN = -216.0f; // slab bottom headroom
static constexpr float SDF_WORLD_Y_MAX = 456.0f;  // > tallest peak (~250) and build ceiling 400
static constexpr int   SDF_CHUNKS_Y = 7;          // ceil((456-(-216))/(32*3)) = 7 layers (672)
// The solid slab has a finite bottom so the map reads as a thick body from the
// side, not an infinitely thin skin. Below this Y everything is open (air).
static constexpr float SDF_FLOOR_Y = -200.0f;     // deep dig floor (slab bottom)

// Sparse chunk: `sdf` is empty until real data is needed; `uniform` is the
// constant field value while empty (positive=air, negative=solid).
struct SDFChunk {
    static constexpr int N = SDF_CHUNK_SIZE + 1; // +1 shared boundary layer
    std::vector<float> sdf;
    float uniform = 1.0f;
    bool dirty = false;
    GLuint vao = 0, vbo = 0;
    int vertex_count = 0;

    inline bool allocated() const { return !sdf.empty(); }
    inline float at(int ly, int lz, int lx) const {
        return sdf.empty() ? uniform : sdf[(ly*N + lz)*N + lx];
    }
    inline void set(int ly, int lz, int lx, float v) {
        sdf[(ly*N + lz)*N + lx] = v;
    }
    void allocate() {
        if (sdf.empty()) sdf.assign(N*N*N, uniform);
    }
};

class SDFTerrain {
public:
    enum class CarveOp : uint8_t { Dig = 0, Fill = 1 };
    struct CarveEvent { glm::vec3 center; float radius; CarveOp op; };

    int chunks_x = 0, chunks_z = 0;
    std::vector<SDFChunk> chunks;
    float world_size = 6000.0f;
    float world_y_min = SDF_WORLD_Y_MIN;
    float world_y_max = SDF_WORLD_Y_MAX;
    float voxel_size = SDF_VOXEL_SIZE;
    int chunk_size = SDF_CHUNK_SIZE;

    Terrain* legacy_terrain = nullptr;
    std::vector<float> orig_height; // original surface snapshot for strata shading
    int ohgs = 0;

    void init(Terrain* terrain, float ws);
    void carve(const CarveEvent& ev);
    void remesh_dirty();
    void render() {
        for (auto& c : chunks) {
            if (c.vertex_count == 0) continue;
            glBindVertexArray(c.vao);
            glDrawArrays(GL_TRIANGLES, 0, c.vertex_count);
        }
        glBindVertexArray(0);
    }
    void cleanup() {
        for (auto& c : chunks) {
            if (c.vao) { glDeleteVertexArrays(1,&c.vao); c.vao=0; }
            if (c.vbo) { glDeleteBuffers(1,&c.vbo); c.vbo=0; }
        }
        chunks.clear();
    }

    SDFChunk& get_chunk(int cx, int cy, int cz) {
        return chunks[(cy*chunks_z + cz)*chunks_x + cx];
    }
    bool in_range(int cx, int cy, int cz) const {
        return cx>=0&&cy>=0&&cz>=0&&cx<chunks_x&&cy<SDF_CHUNKS_Y&&cz<chunks_z;
    }
    void mark_dirty(int cx, int cy, int cz) {
        if (in_range(cx,cy,cz)) get_chunk(cx,cy,cz).dirty = true;
    }

    static float smin(float a, float b, float k) {
        if (k <= 1e-4f) return a < b ? a : b;
        float h = fmaxf(k - fabsf(a-b), 0.0f) / k;
        return fminf(a,b) - h*h*k*0.25f;
    }
    static float smax(float a, float b, float k) { return -smin(-a,-b,k); }

    void world_to_chunk(glm::vec3 wp, int& cx, int& cy, int& cz) const {
        float half = world_size * 0.5f;
        cx = (int)floorf((wp.x + half) / (chunk_size*voxel_size));
        cy = (int)floorf((wp.y - world_y_min) / (chunk_size*voxel_size));
        cz = (int)floorf((wp.z + half) / (chunk_size*voxel_size));
    }
    glm::vec3 voxel_world_pos(int cx,int cy,int cz,int lx,int ly,int lz) const {
        float half = world_size * 0.5f;
        return glm::vec3(
            -half + (cx*chunk_size + lx)*voxel_size,
            world_y_min + (cy*chunk_size + ly)*voxel_size,
            -half + (cz*chunk_size + lz)*voxel_size);
    }

    // Analytic (pristine) field at a world point: the same formula fill_chunk
    // uses. Solid between the slab floor and the surface, air outside.
    inline float analytic_field(float wx, float wy, float wz) const;

    // SDF at a global voxel index (positive=air outside). For chunks that were
    // never allocated (pure air/solid sentinels), return the CONTINUOUS analytic
    // field instead of the 卤50 sentinel - otherwise gradient sampling jumps from
    // ~0 to 卤50 across a meshed<->uniform chunk border and bakes hard lighting
    // seams ("solid lines") exactly at chunk edges.
    float sdf_at_global(int gx,int gy,int gz) {
        int cx=gx/chunk_size, lx=gx%chunk_size;
        int cy=gy/chunk_size, ly=gy%chunk_size;
        int cz=gz/chunk_size, lz=gz%chunk_size;
        if (in_range(cx,cy,cz)) {
            SDFChunk& c = get_chunk(cx,cy,cz);
            if (c.allocated()) return c.sdf[(ly*SDFChunk::N + lz)*SDFChunk::N + lx];
        }
        glm::vec3 wp = voxel_world_pos(cx,cy,cz,lx,ly,lz);
        return analytic_field(wp.x, wp.y, wp.z);
    }
    float sample_sdf(float wx,float wy,float wz) {
        float half = world_size*0.5f;
        float gx=(wx+half)/voxel_size, gy=(wy-world_y_min)/voxel_size, gz=(wz+half)/voxel_size;
        int tx=chunks_x*chunk_size, ty=SDF_CHUNKS_Y*chunk_size, tz=chunks_z*chunk_size;
        gx=std::clamp(gx,0.0f,(float)tx-1.001f);
        gy=std::clamp(gy,0.0f,(float)ty-1.001f);
        gz=std::clamp(gz,0.0f,(float)tz-1.001f);
        int ix=(int)gx, iy=(int)gy, iz=(int)gz;
        float fx=gx-ix, fy=gy-iy, fz=gz-iz;
        float c000=sdf_at_global(ix,iy,iz),     c100=sdf_at_global(ix+1,iy,iz);
        float c010=sdf_at_global(ix,iy+1,iz),   c110=sdf_at_global(ix+1,iy+1,iz);
        float c001=sdf_at_global(ix,iy,iz+1),   c101=sdf_at_global(ix+1,iy,iz+1);
        float c011=sdf_at_global(ix,iy+1,iz+1), c111=sdf_at_global(ix+1,iy+1,iz+1);
        float c00=c000*(1-fx)+c100*fx, c10=c010*(1-fx)+c110*fx;
        float c01=c001*(1-fx)+c101*fx, c11=c011*(1-fx)+c111*fx;
        float c0=c00*(1-fy)+c10*fy, c1=c01*(1-fy)+c11*fy;
        return c0*(1-fz)+c1*fz;
    }
    glm::vec3 sdf_gradient(glm::vec3 wp) {
        // Use finer sampling for normals (0.5m) independent of voxel_size (3m)
        // to avoid blocky/dirty appearance on terrain
        float e=0.5f;
        float dx=sample_sdf(wp.x+e,wp.y,wp.z)-sample_sdf(wp.x-e,wp.y,wp.z);
        float dy=sample_sdf(wp.x,wp.y+e,wp.z)-sample_sdf(wp.x,wp.y-e,wp.z);
        float dz=sample_sdf(wp.x,wp.y,wp.z+e)-sample_sdf(wp.x,wp.y,wp.z-e);
        glm::vec3 g(dx,dy,dz); float L=glm::length(g);
        return L>1e-6f ? g/L : glm::vec3(0,1,0);
    }

    float original_height_at(float wx,float wz) const {
        if (orig_height.empty()) return 0.0f;
        float fx=(wx/world_size+0.5f)*(ohgs-1), fz=(wz/world_size+0.5f)*(ohgs-1);
        int ix=(int)fx, iz=(int)fz;
        ix=std::clamp(ix,0,ohgs-1); iz=std::clamp(iz,0,ohgs-1);
        return orig_height[iz*ohgs+ix];
    }

    // ====================================================================
    // Implementation
    // ====================================================================
    inline void smooth(glm::vec3 center, float radius);
    inline void remesh_chunk(SDFChunk& chunk, int cx, int cy, int cz);
    inline void writeback_heightmap(const CarveEvent& ev);
    inline float find_surface_y(float wx, float wz);
    inline void emit_vertex(std::vector<float>& v, glm::vec3 p, glm::vec3 n);
    inline void fill_chunk(SDFChunk& chunk, int cx, int cy, int cz);
};
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              

// ==========================================================================
// SDFTerrain method definitions
// ==========================================================================
inline void SDFTerrain::fill_chunk(SDFChunk& chunk, int cx, int cy, int cz) {
    // Decide if the surface passes through this chunk. If the chunk is wholly
    // above the surface (all air) or wholly below (all solid), keep it sparse.
    float ymin = world_y_min + cy*chunk_size*voxel_size;
    float ymax = ymin + chunk_size*voxel_size;
    // Sample surface height over the chunk footprint extremes.
    float hmin = 1e9f, hmax = -1e9f;
    for (int lz=0; lz<=chunk_size; lz+=chunk_size)
    for (int lx=0; lx<=chunk_size; lx+=chunk_size) {
        glm::vec3 wp = voxel_world_pos(cx,cy,cz,lx,0,lz);
        float h = legacy_terrain->get_height_at(wp.x, wp.z);
        hmin = fminf(hmin,h); hmax = fmaxf(hmax,h);
    }
    // Margin so neighbouring chunks share a consistent boundary band.
    float margin = voxel_size * 2.0f;
    bool straddles_floor = (ymin < SDF_FLOOR_Y + margin && ymax > SDF_FLOOR_Y - margin);
    if (ymin > hmax + margin) { chunk.uniform = +50.0f; chunk.dirty = false; return; } // all air
    // all solid ONLY if it also does not contain the slab floor (else we must
    // mesh the bottom face so the map has visible thickness from the side).
    if (ymax < hmin - margin && !straddles_floor && ymin > SDF_FLOOR_Y + margin)
        { chunk.uniform = -50.0f; chunk.dirty = false; return; }
    // Entirely below the floor = open air underneath the slab.
    if (ymax < SDF_FLOOR_Y - margin) { chunk.uniform = +50.0f; chunk.dirty = false; return; }
    // Surface crosses: allocate and fill the (gradient-normalized) SDF.
    chunk.allocate();
    for (int ly=0; ly<SDFChunk::N; ly++)
    for (int lz=0; lz<SDFChunk::N; lz++)
    for (int lx=0; lx<SDFChunk::N; lx++) {
        glm::vec3 wp = voxel_world_pos(cx,cy,cz,lx,ly,lz);
        chunk.set(ly,lz,lx, analytic_field(wp.x, wp.y, wp.z));
    }
    chunk.dirty = true;
}

// Pristine analytic field: an APPROXIMATE Euclidean distance to the terrain
// body (solid where SDF_FLOOR_Y <= y <= surface). The raw heightfield diff
// (y - h) is NOT a distance -- on steep slopes |grad h| >> 1, so Marching
// Cubes misplaces zero-crossings and the surface shatters into spikes. We
// divide by sqrt(1+|grad h|^2) to convert it into a true point-to-surface
// distance, which makes MC place clean, well-formed triangles on cliffs.
inline float SDFTerrain::analytic_field(float wx, float wy, float wz) const {
    if (!legacy_terrain) return wy;
    float h = legacy_terrain->get_height_at(wx, wz);
    // Central-difference surface gradient (per world unit).
    float e = voxel_size;
    float hx = legacy_terrain->get_height_at(wx+e, wz) - legacy_terrain->get_height_at(wx-e, wz);
    float hz = legacy_terrain->get_height_at(wx, wz+e) - legacy_terrain->get_height_at(wx, wz-e);
    float gx = hx / (2.0f*e), gz = hz / (2.0f*e);
    float inv = 1.0f / sqrtf(1.0f + gx*gx + gz*gz);
    float d_top = (wy - h) * inv;     // normalized distance above surface
    float d_bot = SDF_FLOOR_Y - wy;   // floor stays axis-aligned (flat)
    return fmaxf(d_top, d_bot);
}

inline void SDFTerrain::init(Terrain* terrain, float ws) {
    legacy_terrain = terrain;
    world_size = ws;
    chunks_x = (int)ceilf(world_size / (chunk_size*voxel_size));
    chunks_z = chunks_x;
    chunks.assign((size_t)chunks_x*chunks_z*SDF_CHUNKS_Y, SDFChunk());
    for (int cy=0; cy<SDF_CHUNKS_Y; cy++)
    for (int cz=0; cz<chunks_z; cz++)
    for (int cx=0; cx<chunks_x; cx++)
        fill_chunk(get_chunk(cx,cy,cz), cx,cy,cz);
    // Original surface snapshot for depth strata shading.
    ohgs = terrain->GRID_SIZE;
    orig_height.resize((size_t)ohgs*ohgs);
    for (int z=0; z<ohgs; z++) for (int x=0; x<ohgs; x++)
        orig_height[z*ohgs+x] = terrain->heights(z,x);
    remesh_dirty();
}

inline void SDFTerrain::carve(const CarveEvent& ev) {
    float r = ev.radius;
    float k = r * 0.5f;                 // smooth-blend width (rounded bowl)
    float reach = r + k + voxel_size;
    glm::vec3 cmin = ev.center - glm::vec3(reach);
    glm::vec3 cmax = ev.center + glm::vec3(reach);
    int cx0,cy0,cz0,cx1,cy1,cz1;
    world_to_chunk(cmin,cx0,cy0,cz0);
    world_to_chunk(cmax,cx1,cy1,cz1);
    // Expand by one chunk so shared boundary voxels are written identically in
    // both neighbours (otherwise meshes do not line up = "can't connect").
    cx0=std::max(0,cx0-1); cy0=std::max(0,cy0-1); cz0=std::max(0,cz0-1);
    cx1=std::min(chunks_x-1,cx1+1); cy1=std::min(SDF_CHUNKS_Y-1,cy1+1); cz1=std::min(chunks_z-1,cz1+1);

    for (int cy=cy0; cy<=cy1; cy++)
    for (int cz=cz0; cz<=cz1; cz++)
    for (int cx=cx0; cx<=cx1; cx++) {
        SDFChunk& chunk = get_chunk(cx,cy,cz);
        chunk.allocate(); // carving needs real data
        bool modified=false;
        for (int ly=0; ly<SDFChunk::N; ly++)
        for (int lz=0; lz<SDFChunk::N; lz++)
        for (int lx=0; lx<SDFChunk::N; lx++) {
            glm::vec3 wp = voxel_world_pos(cx,cy,cz,lx,ly,lz);
            float dist = glm::length(wp - ev.center);
            if (dist > reach) continue;
            float sphere = dist - r;
            float cur = chunk.at(ly,lz,lx);
            float nv = (ev.op==CarveOp::Dig) ? smax(cur,-sphere,k) : smin(cur,sphere,k);
            // Hard limits: never dig through (keep deep voxels solid) and never
            // raise past the ceiling (keep high voxels air). Floor is shallow,
            // ceiling is high so you can build tall but not tunnel to the void.
            // Floor sits ABOVE the slab bottom (SDF_FLOOR_Y=-200) so a solid
            // buffer always remains -- you can dig deep but never punch a hole
            // clean through to the void under the map.
            const float DIG_FLOOR = -160.0f;  // lowest carve depth (40u above slab bottom)
            const float RAISE_CEIL = 400.0f;  // highest build height (~2.5x deeper than dig)
            if (wp.y < DIG_FLOOR)  nv = fminf(nv, cur); // cannot get more airy
            if (wp.y > RAISE_CEIL) nv = fmaxf(nv, cur); // cannot get more solid
            if (nv != cur) { chunk.set(ly,lz,lx,nv); modified=true; }
        }
        if (modified) chunk.dirty = true;
    }
    writeback_heightmap(ev);
}

inline void SDFTerrain::remesh_dirty() {
    for (int cy=0; cy<SDF_CHUNKS_Y; cy++)
    for (int cz=0; cz<chunks_z; cz++)
    for (int cx=0; cx<chunks_x; cx++) {
        SDFChunk& c = get_chunk(cx,cy,cz);
        if (!c.dirty) continue;
        remesh_chunk(c, cx,cy,cz);
        c.dirty = false;
    }
}

static const int SDFT_CORNER[8][3] = {
    {0,0,0},{1,0,0},{1,0,1},{0,0,1},{0,1,0},{1,1,0},{1,1,1},{0,1,1}
};
static const int SDFT_EDGE_V[12][2] = {
    {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
};

inline void SDFTerrain::remesh_chunk(SDFChunk& chunk, int cx, int cy, int cz) {
    if (!chunk.allocated()) { chunk.vertex_count = 0; return; }
    std::vector<float> verts;
    verts.reserve(2048);
    for (int ly=0; ly<chunk_size; ly++)
    for (int lz=0; lz<chunk_size; lz++)
    for (int lx=0; lx<chunk_size; lx++) {
        float cube[8]; glm::vec3 cpos[8]; int ci=0;
        for (int i=0;i<8;i++){
            int ox=SDFT_CORNER[i][0],oy=SDFT_CORNER[i][1],oz=SDFT_CORNER[i][2];
            cube[i]=chunk.at(ly+oy,lz+oz,lx+ox);
            cpos[i]=voxel_world_pos(cx,cy,cz,lx+ox,ly+oy,lz+oz);
            if (cube[i]<0.0f) ci|=(1<<i);
        }
        int edges=MC_EDGE_TABLE[ci];
        if (!edges) continue;
        glm::vec3 vl[12];
        for (int e=0;e<12;e++){
            if(!(edges&(1<<e)))continue;
            int a=SDFT_EDGE_V[e][0],b=SDFT_EDGE_V[e][1];
            float da=cube[a],db=cube[b];
            float t=(fabsf(da-db)<1e-6f)?0.5f:da/(da-db);
            vl[e]=cpos[a]+t*(cpos[b]-cpos[a]);
        }
        const int* tri=MC_TRI_TABLE[ci];
        for(int i=0;tri[i]!=-1;i+=3){
            glm::vec3 p0=vl[tri[i]],p1=vl[tri[i+1]],p2=vl[tri[i+2]];
            emit_vertex(verts,p0,sdf_gradient(p0));
            emit_vertex(verts,p1,sdf_gradient(p1));
            emit_vertex(verts,p2,sdf_gradient(p2));
        }
    }
    chunk.vertex_count=(int)(verts.size()/10);
    if (chunk.vertex_count==0){
        if(chunk.vbo){glDeleteBuffers(1,&chunk.vbo);chunk.vbo=0;}
        if(chunk.vao){glDeleteVertexArrays(1,&chunk.vao);chunk.vao=0;}
        return;
    }
    if(!chunk.vao){
        glGenVertexArrays(1,&chunk.vao); glGenBuffers(1,&chunk.vbo);
        glBindVertexArray(chunk.vao); glBindBuffer(GL_ARRAY_BUFFER,chunk.vbo);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,10*sizeof(float),(void*)0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,10*sizeof(float),(void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
        glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,10*sizeof(float),(void*)(6*sizeof(float))); glEnableVertexAttribArray(2);
        glVertexAttribPointer(3,1,GL_FLOAT,GL_FALSE,10*sizeof(float),(void*)(8*sizeof(float))); glEnableVertexAttribArray(3);
        glVertexAttribPointer(4,1,GL_FLOAT,GL_FALSE,10*sizeof(float),(void*)(9*sizeof(float))); glEnableVertexAttribArray(4);
    } else {
        glBindVertexArray(chunk.vao); glBindBuffer(GL_ARRAY_BUFFER,chunk.vbo);
    }
    glBufferData(GL_ARRAY_BUFFER,verts.size()*sizeof(float),verts.data(),GL_DYNAMIC_DRAW);
    glBindVertexArray(0);
}

inline float SDFTerrain::find_surface_y(float wx, float wz) {
    float top=world_y_max, bot=world_y_min, step=voxel_size*0.5f;
    float prev=sample_sdf(wx,top,wz), prev_y=top;
    for(float y=top-step; y>=bot; y-=step){
        float s=sample_sdf(wx,y,wz);
        if(prev>0.0f && s<=0.0f){ float t=prev/(prev-s); return prev_y+t*(y-prev_y); }
        prev=s; prev_y=y;
    }
    return legacy_terrain ? legacy_terrain->get_height_at(wx,wz) : bot;
}

inline void SDFTerrain::writeback_heightmap(const CarveEvent& ev) {
    if(!legacy_terrain) return;
    float r=ev.radius;
    int gs=legacy_terrain->GRID_SIZE;
    float cell=legacy_terrain->CELL_SIZE;
    int x0=(int)floorf(((ev.center.x-r)/world_size+0.5f)*(gs-1))-1;
    int x1=(int)ceilf (((ev.center.x+r)/world_size+0.5f)*(gs-1))+1;
    int z0=(int)floorf(((ev.center.z-r)/world_size+0.5f)*(gs-1))-1;
    int z1=(int)ceilf (((ev.center.z+r)/world_size+0.5f)*(gs-1))+1;
    x0=std::max(0,x0); z0=std::max(0,z0); x1=std::min(gs-1,x1); z1=std::min(gs-1,z1);
    for(int z=z0;z<=z1;z++)for(int x=x0;x<=x1;x++){
        float wx=((float)x/(gs-1)-0.5f)*world_size;
        float wz=((float)z/(gs-1)-0.5f)*world_size;
        // SDF is authoritative: heightmap follows the marched surface directly
        // (units/camera walk on exactly what is rendered, so no drift / mismatch).
        legacy_terrain->heights(z,x)=find_surface_y(wx,wz);
    }
    for(int z=std::max(1,z0);z<=std::min(gs-2,z1);z++)
    for(int x=std::max(1,x0);x<=std::min(gs-2,x1);x++){
        float hL=legacy_terrain->heights(z,x-1), hR=legacy_terrain->heights(z,x+1);
        float hD=legacy_terrain->heights(z-1,x), hU=legacy_terrain->heights(z+1,x);
        legacy_terrain->normals(z,x)=glm::normalize(glm::vec3(hL-hR,2.0f*cell,hD-hU));
    }
    legacy_terrain->reupload_rows(z0,z1);
}

inline void SDFTerrain::emit_vertex(std::vector<float>& v, glm::vec3 p, glm::vec3 n) {
    float u=(p.x/world_size)+0.5f, vv=(p.z/world_size)+0.5f;
    float surf=original_height_at(p.x,p.z);
    float depth=surf-p.y; // >0 below original ground
    // IMPORTANT: store the SURFACE biome ID unchanged. Encoding dirt/rock here
    // (7/8) made triangles that straddle the depth thresholds interpolate the
    // biome float through phantom IDs 2..6 (water/sand/snow) -> coloured stripes
    // all over the mesh (the bug.png artifact). Dirt/rock are now chosen in the
    // fragment shader from the CONTINUOUS depth channel, which interpolates
    // safely. So biome stays constant per surface column.
    float biome = legacy_terrain ? (float)(uint8_t)legacy_terrain->get_biome_at(p.x,p.z) : 0.0f;
    // height_norm carries DEPTH so the shader can pick dirt/rock and darken.
    float hnorm = depth; // world units below surface (0 at top)
    v.insert(v.end(), {p.x,p.y,p.z, n.x,n.y,n.z, u,vv, biome, hnorm});
}


inline void SDFTerrain::smooth(glm::vec3 center, float radius) {
    float reach = radius + voxel_size;
    glm::vec3 cmin=center-glm::vec3(reach), cmax=center+glm::vec3(reach);
    int cx0,cy0,cz0,cx1,cy1,cz1;
    world_to_chunk(cmin,cx0,cy0,cz0); world_to_chunk(cmax,cx1,cy1,cz1);
    cx0=std::max(0,cx0-1); cy0=std::max(0,cy0-1); cz0=std::max(0,cz0-1);
    cx1=std::min(chunks_x-1,cx1+1); cy1=std::min(SDF_CHUNKS_Y-1,cy1+1); cz1=std::min(chunks_z-1,cz1+1);
    // Read original samples, write a blurred copy (box blur of the SDF field)
    // weighted by distance so only the brush sphere is affected.
    for (int cy=cy0; cy<=cy1; cy++)
    for (int cz=cz0; cz<=cz1; cz++)
    for (int cx=cx0; cx<=cx1; cx++) {
        SDFChunk& chunk = get_chunk(cx,cy,cz);
        chunk.allocate();
        bool modified=false;
        for (int ly=0; ly<SDFChunk::N; ly++)
        for (int lz=0; lz<SDFChunk::N; lz++)
        for (int lx=0; lx<SDFChunk::N; lx++) {
            glm::vec3 wp = voxel_world_pos(cx,cy,cz,lx,ly,lz);
            float dist = glm::length(wp - center);
            if (dist > reach) continue;
            float w = 1.0f - std::clamp(dist/radius, 0.0f, 1.0f);
            if (w <= 0.0f) continue;
            // 6-neighbour average via trilinear samples one voxel away.
            float e=voxel_size;
            float avg = (sample_sdf(wp.x+e,wp.y,wp.z)+sample_sdf(wp.x-e,wp.y,wp.z)
                       + sample_sdf(wp.x,wp.y+e,wp.z)+sample_sdf(wp.x,wp.y-e,wp.z)
                       + sample_sdf(wp.x,wp.y,wp.z+e)+sample_sdf(wp.x,wp.y,wp.z-e))/6.0f;
            float cur = chunk.at(ly,lz,lx);
            float nv = cur + (avg - cur) * w * 0.6f;
            if (nv != cur) { chunk.set(ly,lz,lx,nv); modified=true; }
        }
        if (modified) chunk.dirty = true;
    }
    // keep heightmap in sync over the smoothed footprint
    CarveEvent ev; ev.center=center; ev.radius=radius; ev.op=CarveOp::Dig;
    writeback_heightmap(ev);
}
                                                                                                                                                                                                                                                                         