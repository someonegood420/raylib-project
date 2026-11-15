#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec2 resolution;

uniform vec2 rippleCenter0;
uniform vec2 rippleCenter1;
uniform vec2 rippleCenter2;
uniform vec2 rippleCenter3;

uniform float rippleRadius0;
uniform float rippleRadius1;
uniform float rippleRadius2;
uniform float rippleRadius3;

uniform float rippleStrength0;
uniform float rippleStrength1;
uniform float rippleStrength2;
uniform float rippleStrength3;

out vec4 finalColor;

// Centers use normalized UV coordinates.
// Radius and strength are measured in screen pixels.
vec2 distort(vec2 uv, vec2 center, float radius, float strength)
{
    if (strength <= 0.001)
        return vec2(0.0);

    vec2 delta = (uv - center) * resolution;
    float dist = length(delta);

    if (dist < 0.001)
        return vec2(0.0);

    float ringWidth = 24.0;
    float ringDistance = dist - radius;

    float band = exp(
        -(ringDistance * ringDistance) /
        (2.0 * ringWidth * ringWidth)
    );

    // Stronger test distortion.
    float wave = sin(ringDistance * 0.18) * band;
    float displacement = wave * strength * 10.0;

    return normalize(delta) * displacement / resolution;
}

void main()
{
    vec2 uv = fragTexCoord;
    vec2 offset = vec2(0.0);

    offset += distort(uv, rippleCenter0, rippleRadius0, rippleStrength0);
    offset += distort(uv, rippleCenter1, rippleRadius1, rippleStrength1);
    offset += distort(uv, rippleCenter2, rippleRadius2, rippleStrength2);
    offset += distort(uv, rippleCenter3, rippleRadius3, rippleStrength3);

    // Clamp sampling to avoid reading outside the rendered scene.
    vec2 sampleUV = clamp(uv + offset, vec2(0.001), vec2(0.999));

    finalColor = texture(texture0, sampleUV) * fragColor;
}
