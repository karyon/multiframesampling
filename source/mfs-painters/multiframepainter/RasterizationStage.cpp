#include "RasterizationStage.h"

#include <glm/gtc/matrix_transform.hpp>

#include <glbinding/gl/enum.h>
#include <glbinding/gl/functions.h>
#include <glbinding/gl/boolean.h>

#include <globjects/Framebuffer.h>
#include <globjects/Texture.h>
#include <globjects/Program.h>
#include <globjects/Shader.h>

#include <gloperate/base/make_unique.hpp>
#include <gloperate/painter/AbstractPerspectiveProjectionCapability.h>
#include <gloperate/painter/AbstractViewportCapability.h>
#include <gloperate/painter/AbstractCameraCapability.h>
#include <gloperate/primitives/Icosahedron.h>

#include <gloperate/primitives/PolygonalDrawable.h>

#include <reflectionzeug/property/extensions/GlmProperties.h>

#include "Material.h"
#include "ModelLoadingStage.h"
#include "KernelGenerationStage.h"
#include "MultiFramePainter.h"

using namespace gl;
using gloperate::make_unique;

namespace
{
    enum Sampler
    {
        ShadowSampler,
        MaskSampler,
        NoiseSampler,
        DiffuseSampler,
        SpecularSampler,
        EmissiveSampler,
        OpacitySampler,
        BumpSampler
    };
}

RasterizationStage::RasterizationStage(std::string name, ModelLoadingStage& modelLoadingStage, KernelGenerationStage& kernelGenerationStage, bool renderRSM)
: m_name(name)
, m_modelLoadingStage(modelLoadingStage)
, m_kernelGenerationStage(kernelGenerationStage)
, m_renderRSM(renderRSM)
{
    currentFrame = 1;
}
RasterizationStage::~RasterizationStage()
{
}

void RasterizationStage::initProperties(MultiFramePainter& painter)
{
}

void RasterizationStage::initialize()
{
    setupGLState();

    diffuseBuffer = globjects::Texture::createDefault(GL_TEXTURE_2D);
    specularBuffer = globjects::Texture::createDefault(GL_TEXTURE_2D);
    faceNormalBuffer = globjects::Texture::createDefault(GL_TEXTURE_2D);
    normalBuffer = globjects::Texture::createDefault(GL_TEXTURE_2D);
    vsmBuffer = globjects::Texture::createDefault(GL_TEXTURE_2D);
    depthBuffer = globjects::Texture::createDefault(GL_TEXTURE_2D);

    diffuseBuffer->setName(m_name + " Diffuse");
    specularBuffer->setName(m_name + " Specular");
    faceNormalBuffer->setName(m_name + " Face Normal");
    normalBuffer->setName(m_name + " Normal");
    vsmBuffer->setName(m_name + " VSM");
    depthBuffer->setName(m_name + " Depth");

    m_fbo = new globjects::Framebuffer();
    m_fbo->attachTexture(GL_COLOR_ATTACHMENT0, diffuseBuffer);
    m_fbo->attachTexture(GL_COLOR_ATTACHMENT1, specularBuffer);
    m_fbo->attachTexture(GL_COLOR_ATTACHMENT2, faceNormalBuffer);
    m_fbo->attachTexture(GL_COLOR_ATTACHMENT3, normalBuffer);
    m_fbo->attachTexture(GL_COLOR_ATTACHMENT4, vsmBuffer);
    m_fbo->attachTexture(GL_DEPTH_ATTACHMENT, depthBuffer);

    diffuseBuffer->setParameter(gl::GL_TEXTURE_MIN_FILTER, gl::GL_NEAREST);
    diffuseBuffer->setParameter(gl::GL_TEXTURE_MAG_FILTER, gl::GL_NEAREST);
    specularBuffer->setParameter(gl::GL_TEXTURE_MIN_FILTER, gl::GL_NEAREST);
    specularBuffer->setParameter(gl::GL_TEXTURE_MAG_FILTER, gl::GL_NEAREST);
    faceNormalBuffer->setParameter(gl::GL_TEXTURE_MIN_FILTER, gl::GL_NEAREST);
    faceNormalBuffer->setParameter(gl::GL_TEXTURE_MAG_FILTER, gl::GL_NEAREST);
    normalBuffer->setParameter(gl::GL_TEXTURE_MIN_FILTER, gl::GL_NEAREST);
    normalBuffer->setParameter(gl::GL_TEXTURE_MAG_FILTER, gl::GL_NEAREST);
    depthBuffer->setParameter(gl::GL_TEXTURE_MIN_FILTER, gl::GL_NEAREST);
    depthBuffer->setParameter(gl::GL_TEXTURE_MAG_FILTER, gl::GL_NEAREST);

    vsmBuffer->bind();
    vsmBuffer->setParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    vsmBuffer->setParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glm::vec4 color(0.0);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, (float*)&color);

    if (!m_renderRSM)
        globjects::Shader::globalReplace("#define RENDER_RSM", "#undef RENDER_RSM");
    m_program = new globjects::Program();
    m_program->attach(
        globjects::Shader::fromFile(GL_VERTEX_SHADER, "data/shaders/model.vert"),
        globjects::Shader::fromFile(GL_FRAGMENT_SHADER, "data/shaders/model.frag")
    );
    globjects::Shader::clearGlobalReplacements();

    m_zOnlyProgram = new globjects::Program();
    m_zOnlyProgram->attach(
        globjects::Shader::fromFile(GL_VERTEX_SHADER, "data/shaders/model.vert"),
        globjects::Shader::fromFile(GL_FRAGMENT_SHADER, "data/shaders/empty.frag")
    );
}


void RasterizationStage::loadPreset(const PresetInformation& preset)
{
    camera->setEye(preset.camEye);
    camera->setCenter(preset.camCenter);
    projection->setZNear(preset.nearFar.x);
    projection->setZFar(preset.nearFar.y);
    m_bumpType = preset.bumpType;
    m_focalDist = preset.focalDist;
    m_focalPoint = preset.focalPoint;
}

void RasterizationStage::process()
{
    if (viewport->hasChanged())
        resizeTextures(viewport->width(), viewport->height());

    render();
}

void RasterizationStage::resizeTextures(int width, int height)
{
    diffuseBuffer->image2D(0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    specularBuffer->image2D(0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    faceNormalBuffer->image2D(0, GL_RGB10_A2, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    normalBuffer->image2D(0, GL_RGB10_A2, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    vsmBuffer->image2D(0, GL_RG32F, width, height, 0, GL_RG, GL_FLOAT, nullptr);
    depthBuffer->image2D(0, GL_DEPTH_COMPONENT, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

    m_fbo->printStatus(true);
}

void RasterizationStage::render()
{
    glViewport(viewport->x(),
               viewport->y(),
               viewport->width(),
               viewport->height());

    m_fbo->bind();
    m_fbo->setDrawBuffers({
        GL_COLOR_ATTACHMENT0,
        GL_COLOR_ATTACHMENT1,
        GL_COLOR_ATTACHMENT2,
        GL_COLOR_ATTACHMENT3,
        GL_COLOR_ATTACHMENT4
    });

    auto maxFloat = std::numeric_limits<float>::max();

    m_fbo->clearBuffer(GL_COLOR, 0, glm::vec4(0.0f));
    m_fbo->clearBuffer(GL_COLOR, 1, glm::vec4(0.0f));
    m_fbo->clearBuffer(GL_COLOR, 2, glm::vec4(0.0f));
    m_fbo->clearBuffer(GL_COLOR, 3, glm::vec4(0.0f));
    m_fbo->clearBuffer(GL_COLOR, 4, glm::vec4(0.0));
    m_fbo->clearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);

    auto subpixelSample = m_kernelGenerationStage.antiAliasingKernel[currentFrame - 1];
    auto viewportSize = glm::vec2(viewport->width(), viewport->height());
    auto focalPoint = m_kernelGenerationStage.depthOfFieldKernel[currentFrame - 1] * m_focalPoint;
    focalPoint *= useDOF;

    for (auto program : std::vector<globjects::Program*>{ m_program, m_zOnlyProgram })
    {
        program->setUniform("shadowmap", ShadowSampler);
        program->setUniform("masksTexture", MaskSampler);
        program->setUniform("noiseTexture", NoiseSampler);
        program->setUniform("diffuseTexture", DiffuseSampler);
        program->setUniform("specularTexture", SpecularSampler);
        program->setUniform("emissiveTexture", EmissiveSampler);
        program->setUniform("opacityTexture", OpacitySampler);
        program->setUniform("bumpTexture", BumpSampler);

        program->setUniform("cameraEye", camera->eye());
        program->setUniform("viewProjection", projection->projection() * camera->view());

        // offset needs to be doubled, because ndc range is [-1;1] and not [0;1]
        program->setUniform("ndcOffset", 2.0f * subpixelSample / viewportSize);

        program->setUniform("cocPoint", focalPoint);
        program->setUniform("focalDist", m_focalDist);
    }

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    // zPrepass speeds up crytek sponza due to its low geometric complexity
    if (m_modelLoadingStage.getCurrentPreset() == Preset::CrytekSponza)
        zPrepass();

    m_program->use();

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (auto& pair : m_modelLoadingStage.getDrawablesMap())
    {
        auto materialId = pair.first;
        auto& drawables = pair.second;

        auto& material = m_modelLoadingStage.getMaterialMap().at(materialId);

        bool hasDiffuseTex = material.hasTexture(TextureType::Diffuse);
        bool hasBumpTex = material.hasTexture(TextureType::Bump);
        bool hasSpecularTex = material.hasTexture(TextureType::Specular);
        bool hasEmissiveTex = material.hasTexture(TextureType::Emissive);
        bool hasOpacityTex = material.hasTexture(TextureType::Opacity);

        if (hasDiffuseTex)
        {
            auto tex = material.textureMap().at(TextureType::Diffuse);
            tex->bindActive(DiffuseSampler);
        }

        if (hasSpecularTex)
        {
            auto tex = material.textureMap().at(TextureType::Specular);
            tex->bindActive(SpecularSampler);
        }

        if (hasEmissiveTex)
        {
            auto tex = material.textureMap().at(TextureType::Emissive);
            tex->bindActive(EmissiveSampler);
        }

        if (hasOpacityTex)
        {
            auto tex = material.textureMap().at(TextureType::Opacity);
            tex->bindActive(OpacitySampler);
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
        }

        auto bumpType = BumpType::None;
        if (hasBumpTex)
        {
            bumpType = m_bumpType;
            auto tex = material.textureMap().at(TextureType::Bump);
            tex->bindActive(BumpSampler);
        }

        m_program->setUniform("shininess", material.specularFactor);

        m_program->setUniform("bumpType", static_cast<int>(bumpType));
        m_program->setUniform("useDiffuseTexture", hasDiffuseTex);
        m_program->setUniform("useSpecularTexture", hasSpecularTex);
        m_program->setUniform("useEmissiveTexture", hasEmissiveTex);
        m_program->setUniform("useOpacityTexture", hasOpacityTex);
        m_program->setUniform("model", glm::mat4());

        for (auto& drawable : drawables)
        {
            drawable->draw();
        }
    }


    auto icoMat = glm::mat4();
    icoMat = glm::translate(icoMat, { 0.0f, 1.0f, -0.3f });
    m_program->setUniform("model", icoMat);

    auto ico = globjects::make_ref<gloperate::Icosahedron>(2);
    ico->draw();


    m_program->release();

    m_fbo->unbind();
}

void RasterizationStage::zPrepass()
{
    m_zOnlyProgram->use();

    for (auto& pair : m_modelLoadingStage.getDrawablesMap())
    {
        auto materialId = pair.first;
        auto& drawables = pair.second;

        auto& material = m_modelLoadingStage.getMaterialMap().at(materialId);
        if (material.hasTexture(TextureType::Opacity))
        {
            continue;
        }

        for (auto& drawable : drawables)
        {
            drawable->draw();
        }
    }

    m_zOnlyProgram->release();
}

void RasterizationStage::setupGLState()
{
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
}