#version 330 core
in vec3 v_color;
in vec3 v_normal;
in vec3 v_world_pos;
in float v_part_id;
in vec3 v_local_pos; // model-space pos for stable face UV

out vec4 frag_color;

// MC Steve skin colors
const vec3 SKIN = vec3(0.71, 0.53, 0.36);
const vec3 SKIN_DARK = vec3(0.55, 0.40, 0.27);
const vec3 EYE_WHITE = vec3(0.95, 0.95, 0.95);
const vec3 EYE_PUPIL = vec3(0.12, 0.06, 0.40);
const vec3 MOUTH = vec3(0.35, 0.16, 0.12);
const vec3 STEVE_HAIR = vec3(0.22, 0.13, 0.06);
const vec3 STEVE_SHIRT = vec3(0.0, 0.50, 0.50);
const vec3 STEVE_PANTS = vec3(0.16, 0.12, 0.44);
const vec3 STEVE_SHOES = vec3(0.25, 0.22, 0.20);

// MC Villager colors
const vec3 VILLAGER_ROBE = vec3(0.50, 0.32, 0.15);
const vec3 VILLAGER_ROBE_DARK = vec3(0.38, 0.22, 0.10);
const vec3 VILLAGER_COLLAR = vec3(0.82, 0.80, 0.72);

// Cow colors
const vec3 COW_WHITE = vec3(0.92, 0.88, 0.84);
const vec3 COW_BROWN = vec3(0.30, 0.18, 0.08);

// Weapon
const vec3 IRON = vec3(0.65, 0.68, 0.70);
const vec3 WOOD = vec3(0.45, 0.28, 0.10);

void main() {
    vec3 n = normalize(v_normal);
    int part = int(v_part_id + 0.5);
    // Team detection: red army has g<=0.3, blue has g>0.3
    bool is_red = v_color.g <= 0.32;
    bool is_front = (n.z > 0.8);
    bool is_top = (n.y > 0.8);
    bool is_bottom = (n.y < -0.8);

    vec3 base_color = vec3(0.4);

    // Decode hit flash from v_color alpha trick (encoded in color brightness)
    // Actually we pass hit state via inst_state >= 4, shader handles red tint at end

    if (part == 0) {
        // Body
        if (is_red) {
            base_color = STEVE_SHIRT;
        } else {
            base_color = VILLAGER_ROBE;
            // Collar at top of front face
            if (is_top) base_color = VILLAGER_COLLAR;
        }
    }
    else if (part == 1) {
        // HEAD with face details using model-space coordinates
        // Head box is centered at (0,28,0) with half-extents (4,4,4) for Steve
        // So model-space x: -4 to 4, y: 24 to 32, z: -4 to 4
        // Front face (+Z): use x for horizontal, y for vertical
        
        // Normalize to 0-1 UV on 8x8 pixel face
        // local_pos on front face: x in [-4, 4], y in [24, 32]
        float face_u = (v_local_pos.x + 4.0) / 8.0; // 0 to 1, left to right
        float face_v = (v_local_pos.y - 24.0) / 8.0; // 0 to 1, bottom to top
        
        if (is_red) {
            // Steve head
            if (is_front) {
                base_color = SKIN;
                // Eyes at row 5-6 (from bottom), columns 2-3 and 5-6
                // face_v 0.5-0.75 = eye row, face_u 0.25-0.375 and 0.625-0.75
                bool in_left_eye = (face_u > 0.22 && face_u < 0.40 && face_v > 0.50 && face_v < 0.70);
                bool in_right_eye = (face_u > 0.60 && face_u < 0.78 && face_v > 0.50 && face_v < 0.70);
                
                if (in_left_eye || in_right_eye) {
                    // Pupil in center of eye area
                    bool pupil_l = (face_u > 0.27 && face_u < 0.36 && face_v > 0.53 && face_v < 0.67);
                    bool pupil_r = (face_u > 0.64 && face_u < 0.73 && face_v > 0.53 && face_v < 0.67);
                    if (pupil_l || pupil_r) base_color = EYE_PUPIL;
                    else base_color = EYE_WHITE;
                }
                // Nose: center shadow
                bool nose = (face_u > 0.40 && face_u < 0.60 && face_v > 0.35 && face_v < 0.50);
                if (nose) base_color = SKIN_DARK;
                // Mouth
                bool mouth = (face_u > 0.30 && face_u < 0.70 && face_v > 0.18 && face_v < 0.30);
                if (mouth) base_color = MOUTH;
            } else if (is_top) {
                base_color = STEVE_HAIR;
            } else {
                base_color = STEVE_HAIR;
            }
        } else {
            // Villager head (taller: center (0,29,0), half (4,5,4))
            float vface_u = (v_local_pos.x + 4.0) / 8.0;
            float vface_v = (v_local_pos.y - 24.0) / 10.0;
            
            if (is_front) {
                base_color = SKIN;
                // Villager eyes: small beady at row ~0.6
                bool eye_l = (vface_u > 0.20 && vface_u < 0.35 && vface_v > 0.55 && vface_v < 0.65);
                bool eye_r = (vface_u > 0.65 && vface_u < 0.80 && vface_v > 0.55 && vface_v < 0.65);
                if (eye_l || eye_r) base_color = vec3(0.10, 0.06, 0.03);
                // Thick unibrow
                bool brow = (vface_u > 0.15 && vface_u < 0.85 && vface_v > 0.66 && vface_v < 0.72);
                if (brow) base_color = vec3(0.18, 0.10, 0.04);
                // Nose is a separate mesh part, but darken center for depth
                bool nose_shadow = (vface_u > 0.38 && vface_u < 0.62 && vface_v > 0.30 && vface_v < 0.50);
                if (nose_shadow) base_color = SKIN_DARK;
            } else if (is_top) {
                base_color = SKIN_DARK;
            } else {
                base_color = SKIN_DARK;
            }
        }
    }
    else if (part == 2 || part == 3) {
        if (is_red) {
            base_color = mix(STEVE_SHIRT, SKIN, step(0.5, float(is_bottom)));
        } else {
            base_color = VILLAGER_ROBE;
        }
    }
    else if (part == 4 || part == 5) {
        if (is_red) {
            base_color = STEVE_PANTS;
            if (is_bottom) base_color = STEVE_SHOES;
        } else {
            base_color = VILLAGER_ROBE_DARK;
        }
    }
    else if (part == 6) {
        base_color = is_red ? IRON : WOOD;
    }
    else if (part == 7) {
        base_color = vec3(0.30, 0.20, 0.08);
    }
    else if (part == 8) {
        // Cow body with patches
        float patch = sin(v_local_pos.x * 0.8) * cos(v_local_pos.z * 0.6);
        base_color = mix(COW_WHITE, COW_BROWN, smoothstep(0.1, 0.4, patch));
    }
    else if (part == 9) {
        // Cow head
        base_color = COW_WHITE;
        if (is_front) {
            // Cow face: dark muzzle area
            float mu = (v_local_pos.x + 4.0) / 8.0;
            float mv = (v_local_pos.y - 12.0) / 8.0;
            bool muzzle = (mu > 0.25 && mu < 0.75 && mv > 0.1 && mv < 0.5);
            if (muzzle) base_color = vec3(0.35, 0.30, 0.28);
            // Nostrils
            bool nostril_l = (mu > 0.30 && mu < 0.42 && mv > 0.15 && mv < 0.30);
            bool nostril_r = (mu > 0.58 && mu < 0.70 && mv > 0.15 && mv < 0.30);
            if (nostril_l || nostril_r) base_color = vec3(0.15, 0.10, 0.08);
            // Eyes
            bool eye_l = (mu > 0.15 && mu < 0.30 && mv > 0.55 && mv < 0.70);
            bool eye_r = (mu > 0.70 && mu < 0.85 && mv > 0.55 && mv < 0.70);
            if (eye_l || eye_r) base_color = vec3(0.08, 0.05, 0.03);
        }
    }
    else if (part == 10) {
        base_color = COW_WHITE * 0.88;
    }

    // === MC-style lighting ===
    vec3 light_dir = normalize(vec3(0.4, 0.85, 0.3));
    float NdotL = max(dot(n, light_dir), 0.0);
    float ambient = 0.45;
    
    // Face-based AO (MC style)
    float ao = 1.0;
    if (n.y < -0.5) ao = 0.55;
    else if (abs(n.x) > 0.5) ao = 0.82;
    else if (n.z < -0.5) ao = 0.75;

    vec3 lit = base_color * (ambient + NdotL * 0.55) * ao;

    // Distance fog (subtle, NOT bright)
    float dist = length(v_world_pos.xz) / 2500.0;
    vec3 fog_col = vec3(0.55, 0.62, 0.68);
    lit = mix(lit, fog_col, clamp(dist * dist * 0.3, 0.0, 0.25));

    // Hit flash: detect via high red + low blue
    bool is_hit = (v_color.r > 0.85 && v_color.b < 0.05);
    if (is_hit) {
        lit = mix(lit, vec3(1.0, 0.1, 0.1), 0.55);
    }

    frag_color = vec4(lit, 1.0);
}
