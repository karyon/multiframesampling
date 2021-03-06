#version 330

#extension GL_ARB_shading_language_include : require
#include </data/shaders/common/shadowmapping.glsl>
#include </data/shaders/common/reprojection.glsl>
#include </data/shaders/common/srgb_utils.glsl>

in vec2 v_uv;
in vec3 v_viewRay;

out vec3 outColor;

uniform sampler2D diffuseSampler;
uniform sampler2D specularSampler;
uniform sampler2D faceNormalSampler;
uniform sampler2D normalSampler;
uniform sampler2D depthSampler;
uniform sampler2D shadowmap;
uniform sampler2D giSampler;
uniform sampler2D occlusionSampler;

uniform mat4 projectionMatrix;
uniform mat4 projectionInverseMatrix;
uniform mat4 biasedLightViewProjectionMatrix;
uniform vec3 cameraEye;
uniform mat4 viewMatrix;
uniform mat4 viewInvertedMatrix;
uniform float zFar;
uniform float zNear;
uniform vec2 screenSize;

uniform vec3 worldLightPos;
uniform vec3 lightDirection;
uniform vec3 normalizedInverseLightDirection;
uniform float lightIntensity;
uniform float exposure;


vec3 tonemap(vec3 color)
{
    color *= exposure;
    color /= 1 + color; // that's the reinhard operator
    return color;
}


void main()
{
    float d = linearDepth(depthSampler, v_uv, projectionMatrix);

    vec3 N = texture(normalSampler, v_uv, 0).xyz * 2.0 - 1.0;

    vec3 viewCoord = d * v_viewRay;
    vec3 worldCoord = (viewInvertedMatrix * vec4(viewCoord, 1.0)).xyz;

    vec3 L = normalizedInverseLightDirection;
    vec3 V = normalize(cameraEye - worldCoord);
    vec3 H = normalize(L + V);
    float ndotl = dot(N, L);
    float ndotH = dot(N, H);

    vec4 scoord = biasedLightViewProjectionMatrix * vec4(worldCoord, 1.0);


    float shadowFactor = shadowmapComparisonVSM(shadowmap, scoord.xy/scoord.w, worldCoord, worldLightPos);
    shadowFactor *= step(0.0, sign(scoord.w));


    vec3 diffuseColor = toLinear(texture(diffuseSampler, v_uv, 0).xyz);
    vec3 specularColor = toLinear(texture(specularSampler, v_uv, 0).xyz);
    vec3 giColor = texture(giSampler, v_uv, 0).xyz;
    float occlusionFactor = texture(occlusionSampler, v_uv, 0).x;
    vec3 ambientTerm = giColor * diffuseColor * occlusionFactor;
    vec3 diffuseTerm = diffuseColor * (max(0.0, ndotl) * shadowFactor) * lightIntensity;
    const float specularFactor = 0.75;
    vec3 specularTerm = specularFactor * specularColor * pow(max(0.0, ndotH), 20.0) * shadowFactor;

    outColor = ambientTerm + diffuseTerm + specularTerm;
    outColor = tonemap(outColor);
    outColor = toSRGB(outColor);
}
