#include "include/base_vs_types.hlsl"

#if defined(LEFT_SUBSAMPLING)
vertex_t generate_fullscreen_triangle_vertex(uint vertex_id, float subsample_offset, int rotate_texture_steps)
#elif defined(TOPLEFT_SUBSAMPLING)
vertex_t generate_fullscreen_triangle_vertex(uint vertex_id, float2 subsample_offset, int rotate_texture_steps)
#elif defined(RECOMBINED444_V_SAMPLING)
vertex_t generate_fullscreen_triangle_vertex(uint vertex_id, float2 sample_offset, bool second_stack, int rotate_texture_steps)
#else
vertex_t generate_fullscreen_triangle_vertex(uint vertex_id, int rotate_texture_steps)
#endif
{
    vertex_t output;
    float2 tex_coord;

    if (vertex_id == 0) {
        output.viewpoint_pos = float4(-1, -1, 0, 1);
        tex_coord = float2(0, 1);
    }
    else if (vertex_id == 1) {
        output.viewpoint_pos = float4(-1, 3, 0, 1);
        tex_coord = float2(0, -1);
    }
    else {
        output.viewpoint_pos = float4(3, -1, 0, 1);
        tex_coord = float2(2, 1);
    }

    if (rotate_texture_steps != 0) {
        float rotation_radians = radians(90 * rotate_texture_steps);
        float2x2 rotation_matrix = { cos(rotation_radians), -sin(rotation_radians),
                                     sin(rotation_radians), cos(rotation_radians) };
        float2 rotation_center = { 0.5, 0.5 };
        tex_coord = round(rotation_center + mul(rotation_matrix, tex_coord - rotation_center));
    }

#if defined(LEFT_SUBSAMPLING)
    output.tex_right_left_center = float3(tex_coord.x, tex_coord.x - subsample_offset, tex_coord.y);
#elif defined (TOPLEFT_SUBSAMPLING)
    output.tex_right_left_top = float3(tex_coord.x, tex_coord.x - subsample_offset.x, tex_coord.y - subsample_offset.y);
    output.tex_right_left_bottom = float3(tex_coord.x, tex_coord.x - subsample_offset.x, tex_coord.y);
#elif defined(RECOMBINED444_V_SAMPLING)
    // 0   1
    //   x
    // 2   3
    // if (!second_stack) { left = 0; right = 1 }
    // else { left = 2; right = 3 }
    output.tex_right_left_center = float3(tex_coord.x + sample_offset.x,
                                          tex_coord.x - sample_offset.x,
                                          second_stack ? tex_coord.y + sample_offset.y :
                                                         tex_coord.y - sample_offset.y);
#else
    output.tex_coord = tex_coord;
#endif

    return output;
}
