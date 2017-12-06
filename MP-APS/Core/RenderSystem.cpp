#include "RenderSystem.h"

#include "../Graphics/GLShader.h"

#include "../Input.h"
#include "../SceneBase.h"
#include "../ResourceManager.h"

#include <pugixml.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <GLFW/glfw3.h>

#include <random>
#include <iostream>

/***********************************************************************************/
RenderSystem::RenderSystem() :	m_width{0},
							m_height{0},
							m_workGroupsX{0},
							m_workGroupsY{0} {}

/***********************************************************************************/
void RenderSystem::Init(const pugi::xml_node& rendererNode) {
	
	if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
		std::cerr << "Failed to start GLAD.";
		std::abort();
	}

#ifdef _DEBUG
	std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << '\n';
	std::cout << "GLSL Version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << '\n';
	std::cout << "OpenGL Vendor: " << glGetString(GL_VENDOR) << '\n';
	std::cout << "OpenGL Renderer: " << glGetString(GL_RENDERER) << '\n';
#endif

	const auto width = rendererNode.attribute("width").as_uint();
	const auto height = rendererNode.attribute("height").as_uint();

	m_width = width;
	m_height = height;

	m_workGroupsX = (width + width % 8) / 8;
	m_workGroupsY = (height + height % 8) / 8;

	m_depthFBO = std::make_unique<GLFramebuffer>("Depth FBO", width, height);
	m_hdrFBO = std::make_unique<GLFramebuffer>("HDR FBO", width, height);
	m_skybox = std::make_unique<Skybox>("Data/hdri/barcelona.hdr", 1024);

	// Compile all shader programs
	for (auto program = rendererNode.child("Program"); program; program = program.next_sibling("Program")) {

		// Get all shader files that make up the program
		std::vector<GLShader> shaders;
		for (auto shader = program.child("Shader"); shader; shader = shader.next_sibling("Shader")) {
			shaders.emplace_back(shader.attribute("path").as_string(), shader.attribute("type").as_string());
		}

		// Compile and cache shader program
		m_shaderCache.try_emplace(program.attribute("name").as_string(), program.attribute("name").as_string(), shaders);
	}

	glFrontFace(GL_CCW);
	glCullFace(GL_BACK);
	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glEnable(GL_FRAMEBUFFER_SRGB);
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// Create uniform buffer object for projection and view matrices (same data shared to multiple shaders)
	glGenBuffers(1, &m_uboMatrices);
	glBindBuffer(GL_UNIFORM_BUFFER, m_uboMatrices);
	glBufferData(GL_UNIFORM_BUFFER, 2 * sizeof(glm::mat4), nullptr, GL_STATIC_DRAW);
	glBindBufferRange(GL_UNIFORM_BUFFER, 0, m_uboMatrices, 0, 2 * sizeof(glm::mat4));

	setupLightBuffers();
	setupScreenquad();
	setupDepthBuffer();
	setupHDRBuffer();

	glViewport(0, 0, width, height);
}

/***********************************************************************************/
void RenderSystem::Shutdown() const {
	for (const auto& shader : m_shaderCache) {
		shader.second.DeleteProgram();
	}
}

/***********************************************************************************/
void RenderSystem::Render(const SceneBase& scene) {
	const auto viewMatrix = scene.GetCamera().GetViewMatrix();

	glBindBuffer(GL_UNIFORM_BUFFER, m_uboMatrices);

	// Get the shaders we need (static vars initialized during first render call).
	static auto& depthShader = m_shaderCache.at("DepthPassShader");
	static auto& lightCullShader = m_shaderCache.at("LightCullShader");
	static auto& pbrShader = m_shaderCache.at("PBRShader");
	static auto& postProcessShader = m_shaderCache.at("PostProcessShader");

	/*
	m_forwardShader.Bind();
	m_forwardShader.SetUniform("viewPos", camera.GetPosition());
	m_forwardShader.SetUniform("light.direction", renderData.Sun.Direction);
	m_forwardShader.SetUniform("light.ambient", { 0.1f, 0.1f, 0.1f });
	m_forwardShader.SetUniform("light.diffuse", renderData.Sun.Color);
	m_forwardShader.SetUniform("light.specular", glm::vec3(1.0f));
	*/
	
	// Step 1: Render the depth of the scene to a depth map
	depthShader.Bind();
	
	m_depthFBO->Bind();
	glClear(GL_DEPTH_BUFFER_BIT);
	renderModels(depthShader, scene.m_renderList, true);
	m_depthFBO->Unbind();

	// Step 2: Perform light culling on point lights in the scene
	lightCullShader.Bind();
	lightCullShader.SetUniform("projection", m_projMatrix);
	lightCullShader.SetUniform("view", viewMatrix);

	glActiveTexture(GL_TEXTURE5);
	lightCullShader.SetUniformi("depthMap", 5);
	glBindTexture(GL_TEXTURE_2D, m_depthTexture);

	// Bind shader storage buffer objects for the light and index buffers
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_lightBuffer);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_visibleLightIndicesBuffer);

	// Execute compute shader
	glDispatchCompute(m_workGroupsX, m_workGroupsY, 1);

	m_hdrFBO->Bind();
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	//m_lightAccumShader.Bind();
	//m_lightAccumShader.SetUniform("viewPosition", camera.GetPosition());

	// Bind pre-computed IBL data
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_CUBE_MAP, m_skybox->GetIrradianceMap());

	pbrShader.Bind();
	pbrShader.SetUniformi("irradianceMap", 0).SetUniformi("wireframe", true);
	pbrShader.SetUniform("viewPos", scene.GetCamera().GetPosition());
	pbrShader.SetUniform("sunDirection", glm::vec3(scene.m_sun.Direction));
	pbrShader.SetUniform("sunColor", glm::vec3(scene.m_sun.Color));
	renderModels(pbrShader, scene.m_renderList, false);

	// Draw skybox
	m_skybox->Draw();
	
	// Do post-processing
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	postProcessShader.Bind();
	postProcessShader.SetUniformf("vibranceAmount", m_vibrance);
	postProcessShader.SetUniform("vibranceCoefficient", m_coefficient);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_hdrColorBufferTexture);

	renderQuad();
}

/***********************************************************************************/
// TODO: This needs to be gutted and put elsewhere
void RenderSystem::InitView(const Camera& camera) {
	m_projMatrix = camera.GetProjMatrix(m_width, m_height);
	glBindBuffer(GL_UNIFORM_BUFFER, m_uboMatrices);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(glm::mat4), value_ptr(m_projMatrix));
}

/***********************************************************************************/
void RenderSystem::Update(const Camera& camera, const double delta) {

	// Window size changed.
	if (Input::GetInstance().ShouldResize()) {
		m_width = Input::GetInstance().GetWidth();
		m_height = Input::GetInstance().GetHeight();

		m_projMatrix = camera.GetProjMatrix(m_width, m_height);
		InitView(camera);
		glViewport(0, 0, m_width, m_height);
		m_depthFBO->Resize(m_width, m_height);
	}

	// Update view matrix inside UBO
	const auto view = camera.GetViewMatrix();
	glBindBuffer(GL_UNIFORM_BUFFER, m_uboMatrices);
	glBufferSubData(GL_UNIFORM_BUFFER, sizeof(glm::mat4), sizeof(glm::mat4), glm::value_ptr(view));

	UpdateLights(delta);

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/***********************************************************************************/
void RenderSystem::renderModels(GLShaderProgram& shader, const std::vector<ModelPtr>& renderList, const bool depthPass) const {

	for (const auto& model : renderList) {
		shader.SetUniform("modelMatrix", model->GetModelMatrix());

		auto meshes = model->GetMeshes();
		for (auto& mesh : meshes) {

			if (!depthPass) {
				shader.SetUniform("albedo", model->GetMaterial().Albedo);
				shader.SetUniformf("metallic", model->GetMaterial().Metallic);
				shader.SetUniformf("ao", model->GetMaterial().AO);
				shader.SetUniformf("roughness", model->GetMaterial().Roughness);

			}
			mesh.GetVAO().Bind();
			glDrawElements(GL_TRIANGLES, mesh.GetIndexCount(), GL_UNSIGNED_INT, nullptr);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
	}
}

/***********************************************************************************/
void RenderSystem::renderQuad() const {
	m_quadVAO.Bind();
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/***********************************************************************************/
void RenderSystem::setupScreenquad() {
	const std::array<Vertex, 4> screenQuadVertices {
		// Positions				// GLTexture Coords
		Vertex({ -1.0f, 1.0f, 0.0f },{ 0.0f, 1.0f }),
		Vertex({ -1.0f, -1.0f, 0.0f },{ 0.0f, 0.0f }),
		Vertex({ 1.0f, 1.0f, 0.0f },{ 1.0f, 1.0f }),
		Vertex({ 1.0f, -1.0f, 0.0f },{ 1.0f, 0.0f })
	};

	m_quadVAO.Init();
	m_quadVAO.Bind();
	m_quadVAO.AttachBuffer(GLVertexArray::BufferType::ARRAY, 
		sizeof(Vertex) * screenQuadVertices.size(), 
		GLVertexArray::DrawMode::STATIC, 
		screenQuadVertices.data());
	m_quadVAO.EnableAttribute(0, 3, sizeof(Vertex), nullptr);
	m_quadVAO.EnableAttribute(1, 2, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, TexCoords)));
}

/***********************************************************************************/
void RenderSystem::setupLightBuffers() {
	const auto numberOfTiles = m_workGroupsX * m_workGroupsY;

	glGenBuffers(1, &m_lightBuffer);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightBuffer);
	glBufferData(GL_SHADER_STORAGE_BUFFER, MAX_NUM_LIGHTS * sizeof(PointLight), nullptr, GL_DYNAMIC_DRAW);

	glGenBuffers(1, &m_visibleLightIndicesBuffer);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_visibleLightIndicesBuffer);
	glBufferData(GL_SHADER_STORAGE_BUFFER, numberOfTiles * sizeof(VisibleLightIndex) * MAX_NUM_LIGHTS, nullptr, GL_STATIC_DRAW);

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	setupLightStorageBuffer();
}

/***********************************************************************************/
void RenderSystem::setupLightStorageBuffer() {
	if (m_lightBuffer == 0) {
		std::cerr << "Error: Forward+ light buffer has not been created.\n";
		std::abort();
	}

	std::random_device rd;
	std::mt19937_64 gen(rd());
	std::uniform_real_distribution<> dist(0.0f, 1.0f);

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightBuffer);
	auto* pointLights = (PointLight*)glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_READ_WRITE);
	
	for (auto i = 0; i < MAX_NUM_LIGHTS; i++) {
		auto& light = pointLights[i];
		light.Position = glm::vec4(RandomPosition(dist, gen), 1.0f);
		light.Color = glm::vec4(1.0f + dist(gen), 1.0f + dist(gen), 1.0f + dist(gen), 1.0f);
		light.RadiusAndPadding = glm::vec4(glm::vec3(0.0f), 30.0f);
	}

	glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

/***********************************************************************************/
void RenderSystem::setupDepthBuffer() {
	m_depthFBO->Bind();

	if (m_depthTexture) {
		glDeleteTextures(1, &m_depthTexture);
	}
	glGenTextures(1, &m_depthTexture);
	glBindTexture(GL_TEXTURE_2D, m_depthTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, m_width, m_height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	const GLfloat borderColor[] { 1.0f, 1.0f, 1.0f, 1.0f };
	glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

	m_depthFBO->AttachTexture(m_depthTexture, GLFramebuffer::AttachmentType::DEPTH);
	m_depthFBO->DrawBuffer(GLFramebuffer::GLBuffer::NONE);
	m_depthFBO->ReadBuffer(GLFramebuffer::GLBuffer::NONE);
	
	m_depthFBO->Unbind();
}

/***********************************************************************************/
void RenderSystem::setupHDRBuffer() {
	
	m_hdrFBO->Reset(m_width, m_height);
	m_hdrFBO->Bind();
	GLuint rboDepth;
	glGenTextures(1, &m_hdrColorBufferTexture);
	glBindTexture(GL_TEXTURE_2D, m_hdrColorBufferTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_width, m_height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glGenRenderbuffers(1, &rboDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, rboDepth);

	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, m_width, m_height);
	// - Attach buffers
	m_hdrFBO->AttachTexture(m_hdrColorBufferTexture, GLFramebuffer::AttachmentType::COLOR0);
	m_hdrFBO->AttachRenderBuffer(rboDepth, GLFramebuffer::AttachmentType::DEPTH);
}

/***********************************************************************************/
glm::vec3 RenderSystem::RandomPosition(std::uniform_real_distribution<> dis, std::mt19937_64 gen) {
	auto position = glm::vec3(0.0);
	for (auto i = 0; i < 3; i++) {
		const auto min = LIGHT_MIN_BOUNDS[i];
		const auto max = LIGHT_MAX_BOUNDS[i];
		position[i] = (GLfloat)dis(gen) * (max - min) + min;
	}

	return position;
}

/***********************************************************************************/
void RenderSystem::UpdateLights(const double dt) {
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightBuffer);
	PointLight *pointLights = (PointLight*)glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_READ_WRITE);

	for (int i = 0; i < MAX_NUM_LIGHTS; i++) {
		PointLight &light = pointLights[i];
		float min = LIGHT_MIN_BOUNDS[1];
		float max = LIGHT_MAX_BOUNDS[1];

		light.Position.y = fmod((light.Position.y + (-4.5f * dt) - min + max), max) + min;
	}

	glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}