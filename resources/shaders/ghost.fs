#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;

out vec4 finalColor;

void main()
{
    vec4 texel = texture(texture0, fragTexCoord);

    // Use the draw tint (pink) and preserve the sprite's transparency.
    finalColor = vec4(fragColor.rgb, texel.a * fragColor.a);
}
