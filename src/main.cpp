#include <cstdlib>
#include <iostream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <expected>
#include <SDL.h>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

struct vertex_t {
	glm::vec3 position;
    glm::vec3 color;
};

struct depth_cloud_result_t {
    std::vector<vertex_t> vertices;
    float max_depth;
};

struct mesh_t {
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
};

struct camera_t {
    glm::vec3 origin;
    float distance;
    float pitch;
    float yaw;
    float rotate_speed;
    float pan_speed;
    float zoom_scale;
};

static std::expected<std::string, std::string> read_file_contents(const std::filesystem::path& path) {
    std::ifstream ifs(path);

    if (ifs.fail()) 
        return std::unexpected("Failed to read contents of file " + path.string());

    return std::string(
        (std::istreambuf_iterator<char>(ifs)),
        (std::istreambuf_iterator<char>())
    );
}

static std::expected<GLuint, std::string> create_shader(GLenum type, const std::string& src) {
    auto id = glCreateShader(type);

    const char* src_cstr = src.c_str();
    glShaderSource(id, 1, &src_cstr, nullptr);
    glCompileShader(id);

    GLint success;
    glGetShaderiv(id, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLsizei log_length;
        glGetShaderiv(id, GL_INFO_LOG_LENGTH, &log_length);

        std::string log;
        if (log_length > 0) {
            log.resize(log_length);
            glGetShaderInfoLog(id, log_length, nullptr, &log[0]);
        }

        // Cleanup resource if it fails to compile.
        glDeleteShader(id);

        return std::unexpected("Shader compilation error: " + log);
    }

    return id;
}

static std::expected<GLuint, std::string> create_program(GLuint vs, GLuint fs) {
    auto program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        GLsizei log_length;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);

        std::string log;
        if (log_length > 0) {
            log.resize(log_length);
            glGetProgramInfoLog(program, log_length, nullptr, &log[0]);
        }

        // Cleanup resource if it fails to link.
        glDeleteProgram(program);

        return std::unexpected("Shader program linking error: " + log);
    }

    return program;
}

static std::expected<GLuint, std::string> create_program(
    const std::filesystem::path& vs_path,
    const std::filesystem::path& fs_path
) {
    const auto vs_src = read_file_contents(vs_path);
    if (!vs_src) return std::unexpected(vs_src.error());

    const auto fs_src = read_file_contents(fs_path);
    if (!fs_src) return std::unexpected(fs_src.error());

    const auto vs = create_shader(GL_VERTEX_SHADER, *vs_src);
    if (!vs) return std::unexpected(vs.error());

    const auto fs = create_shader(GL_FRAGMENT_SHADER, *fs_src);
    // Be sure to delete vs, if fs fails.
    if (!fs) {
        glDeleteShader(*vs);
        return std::unexpected(fs.error());
    }

    const auto program = create_program(*vs, *fs);
    // Delete now we have made program. This will also delete if program fails too.
    glDeleteShader(*vs);
    glDeleteShader(*fs);
    if (!program)
        return std::unexpected(program.error());
    return program;
}

static std::expected<depth_cloud_result_t, std::string> generate_depth_cloud(
    const std::filesystem::path& image_path,
    const std::filesystem::path& depth_path,
    float focal_length,
    unsigned int stride
) {
    int w1, h1, ch1, w2, h2, ch2;
    unsigned char* pixels1 = stbi_load(image_path.string().c_str(), &w1, &h1, &ch1, 3);
    unsigned short* pixels2 = stbi_load_16(depth_path.string().c_str(), &w2, &h2, &ch2, 3);

    if (!pixels1 || !pixels2) 
        return std::unexpected("Failed to read image or depth map.");

    if (w1 != w2 || h1 != h2 || ch1 != 3 || ch2 != 1) {
        stbi_image_free(pixels1);
        stbi_image_free(pixels2);
        return std::unexpected("Image and depth map not same resolution or invalid channel count.");
    }

    const float center_w = static_cast<float>(w1) * .5f;
    const float center_h = static_cast<float>(h1) * .5f;

    std::vector<vertex_t> vertices = {};
    vertices.reserve(w1 * h1);

    float max_depth = 0.f;

    for (int v = 0; v < h1; v += stride){
        for (int u = 0; u < w1; u += stride) {
            if (v >= h1 || u >= w1)
                continue;

            const int index = (u + v * w1) * ch1;
            const unsigned char r = pixels1[index];
            const unsigned char g = pixels1[index + 1];
            const unsigned char b = pixels1[index + 2];
            // Not sure why this index works here even though it's single channel.
            const unsigned short gray = pixels2[index];

            // ZoeDepth maps are metric scaled down by 255.
            const float depth = static_cast<float>(gray) / 255.0f;

            if (depth > max_depth)
                max_depth = depth;

            glm::vec3 position(
                depth * (w1 - u - center_w) / focal_length,
                depth * (h1 - v - center_h) / focal_length,
                depth
            );

            glm::vec3 color(
                static_cast<float>(r) / 255.f,
                static_cast<float>(g) / 255.f,
                static_cast<float>(b) / 255.f
            );

            vertices.emplace_back(position, color);
        }
    }

    stbi_image_free(pixels1);
    stbi_image_free(pixels2);

    return depth_cloud_result_t { vertices, max_depth };
}

glm::vec3 get_camera_front(float pitch, float yaw) {
    auto front = glm::vec3();
    front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    front.y = sin(glm::radians(pitch));
    front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    return glm::normalize(front);
}

int main(int argc, char** argv) {
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		std::cerr << "SDL initialization failure: " << SDL_GetError() << std::endl;
		return EXIT_FAILURE;
	}

    unsigned int window_width = 1600;
    unsigned int window_height = 900;
    const auto window = SDL_CreateWindow(
        "Window",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
        window_width, window_height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL
    );
	if (!window) {
		std::cerr << "SDL window creation failure: " << SDL_GetError() << std::endl;
		return EXIT_FAILURE;
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    const auto gl_ctx = SDL_GL_CreateContext(window);
	if (!gl_ctx) {
		std::cerr << "SDL GL context creation failure: " << SDL_GetError() << std::endl;
		return EXIT_FAILURE;
	}
	
	if (!gladLoadGLLoader(SDL_GL_GetProcAddress)) {
		std::cerr << "GLAD loading failure." << std::endl;
		return EXIT_FAILURE;
	}

    glEnable(GL_DEPTH_TEST);

    const auto shader_program_result = create_program("shader.vs", "shader.fs");
    if (!shader_program_result) {
        std::cerr << "Failed to create shader program: " << shader_program_result.error() << std::endl;
        return EXIT_FAILURE;
    }
    const auto shader_program = *shader_program_result;
    glUseProgram(shader_program);

    const auto projection_uniform = glGetUniformLocation(shader_program, "projection_matrix");
    const auto view_uniform = glGetUniformLocation(shader_program, "view_matrix");
    const auto model_uniform = glGetUniformLocation(shader_program, "model_matrix");

    const mesh_t cube_mesh{
        .vertices = {
            -1, -1, -1,
            1, -1, -1,
            1, 1, -1,
            -1, 1, -1,
            -1, -1, 1,
            1, -1, 1,
            1, 1, 1,
            -1, 1, 1
        },
        .indices = {
            0, 1, 3,
            3, 1, 2,
            1, 5, 2,
            2, 5, 6,
            5, 4, 6,
            6, 4, 7,
            4, 0, 7,
            7, 0, 3,
            3, 2, 7,
            7, 2, 6,
            4, 5, 0,
            0, 5, 1
        }
    };

	GLuint vao, mesh_vbo, mesh_ebo, point_cloud_vbo;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

    glGenBuffers(1, &mesh_vbo);
    glGenBuffers(1, &mesh_ebo);
    glGenBuffers(1, &point_cloud_vbo);

    glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, cube_mesh.vertices.size() * sizeof(float), cube_mesh.vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, cube_mesh.indices.size() * sizeof(unsigned int), cube_mesh.indices.data(), GL_STATIC_DRAW);

    // TODO: Fix sizing
    // Cube mesh vertex position.
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), 0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, point_cloud_vbo);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(vertex_t), 0);
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(vertex_t), reinterpret_cast<void*>(sizeof(glm::vec3)));
    glEnableVertexAttribArray(2);

    // Position
    glVertexAttribDivisor(1, 1);
    // Color
    glVertexAttribDivisor(2, 1);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 410");

    camera_t camera {
        .origin = glm::vec3(0.f, 0.f, 0.f),
        .distance = 5.f,
        .pitch = 0.f,
        .yaw = 90.f,
        // TODO: Scale by distance.
        .rotate_speed = .1f,
        .pan_speed = .005f,
        .zoom_scale = .1f
    };


    std::optional<std::vector<vertex_t>> vertices = std::nullopt;
    char image_file_str[128] = "";
    char depth_file_str[128] = "";
    auto focal_length = 1400.f;
    auto stride = 4;
    auto background_color = glm::vec3();
    float voxel_scale = .01f;

    bool holding_mouse1 = false;
    bool holding_mouse2 = false;
    bool imgui_window_open = true;

    // TODO: Move some of these
    SDL_Event event;
    Uint32 prev_time, cur_time = 0;
    std::string last_error_message = "";
    bool running = true;

    while (running) {
        prev_time = cur_time;
        cur_time = SDL_GetTicks();
        const auto fps = 1.f / (static_cast<float>(cur_time - prev_time) / 1000.f);

        const auto front = get_camera_front(camera.pitch, camera.yaw);
        const auto up = glm::vec3(0.f, 1.f, 0.f);
        const auto right = glm::normalize(glm::cross(front, up));
        const auto camera_pos = camera.origin - front * camera.distance;

		while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);

            const auto x_rel = static_cast<float>(event.motion.xrel);
            const auto y_rel = static_cast<float>(event.motion.yrel);

			switch (event.type) {
			case SDL_QUIT:
				running = false;
				break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    window_width = event.window.data1;
                    window_height = event.window.data2;
                    glViewport(0, 0, window_width, window_height);
                }
                break;
            case SDL_MOUSEMOTION:
                if (io.WantCaptureMouse)
                    break;

                if (holding_mouse1) {
                    camera.yaw += x_rel * camera.rotate_speed;
                    camera.pitch += -y_rel * camera.rotate_speed;
                    camera.pitch = std::clamp(camera.pitch, -89.f, 89.f);
                }

                if (holding_mouse2) {
                    camera.origin += right * -x_rel * camera.pan_speed;
                    camera.origin += up * y_rel * camera.pan_speed;
                }

                break;
            case SDL_MOUSEWHEEL:
                if (event.wheel.y > 0)
                    camera.distance = camera.distance / (1 + camera.zoom_scale);
                else if (event.wheel.y < 0)
                    camera.distance = camera.distance * (1 + camera.zoom_scale);

                camera.distance = std::max(0.f, camera.distance);
                break;
            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT)
                    holding_mouse1 = false;
                else if (event.button.button == SDL_BUTTON_RIGHT)
                    holding_mouse2 = false;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT)
                    holding_mouse1 = true;
                else if (event.button.button == SDL_BUTTON_RIGHT)
                    holding_mouse2 = true;
                break;
			default:
				break;
			}
		}

		glClearColor(background_color.x, background_color.y, background_color.z, 1.f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        const auto aspect_ratio = static_cast<float>(window_width) / window_height;
        const auto projection_mat = glm::perspective(glm::radians(65.0f), aspect_ratio, 0.1f, 1000.0f);
        const auto view_mat = glm::lookAt(camera_pos, camera.origin, glm::vec3(0.f, 1.f, 0.f));
        auto model_mat = glm::mat4(1.f);
        model_mat = glm::scale(model_mat, glm::vec3(voxel_scale, voxel_scale, voxel_scale));

        glUniformMatrix4fv(projection_uniform, 1, GL_FALSE, glm::value_ptr(projection_mat));
        glUniformMatrix4fv(view_uniform, 1, GL_FALSE, glm::value_ptr(view_mat));
        glUniformMatrix4fv(model_uniform, 1, GL_FALSE, glm::value_ptr(model_mat));

        if (vertices)
            glDrawElementsInstanced(GL_TRIANGLES, cube_mesh.indices.size(), GL_UNSIGNED_INT, 0, (*vertices).size());

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Window", &imgui_window_open);
        {
            if (ImGui::CollapsingHeader("Information"), ImGuiTreeNodeFlags_DefaultOpen) {
                ImGui::Text("FPS: %.2f", fps);
                if (vertices)
                    ImGui::Text("Number of Vertices: %i", (*vertices).size());
            }

            if (ImGui::CollapsingHeader("Generate"), ImGuiTreeNodeFlags_DefaultOpen) {
                ImGui::InputText("Image File", image_file_str, IM_ARRAYSIZE(image_file_str));
                ImGui::InputText("Depth Map File", depth_file_str, IM_ARRAYSIZE(depth_file_str));
                ImGui::SliderFloat("Focal Length", &focal_length, 0.0f, 10000.0f, "%.1f");
                ImGui::SliderInt("Stride", &stride, 1, 10);

                if (ImGui::Button("Generate##2")) {
                    const auto result = generate_depth_cloud(image_file_str, depth_file_str, focal_length, stride);
                    if (!result) {
                        last_error_message = result.error();
                        ImGui::OpenPopup("Error");
                    }
                    else {
                        vertices = (*result).vertices;
                        // Set the center of the point cloud as our origin.
                        camera.origin = glm::vec3(0.f, 0.f, (*result).max_depth);
                        glBufferData(GL_ARRAY_BUFFER, (*vertices).size() * sizeof(vertex_t), (*vertices).data(), GL_STATIC_DRAW);
                    }
                }
            }

            if (ImGui::CollapsingHeader("Settings"), ImGuiTreeNodeFlags_DefaultOpen) {
                ImGui::Text("Background Color");
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::ColorPicker3("Background Color", &background_color.x);
                ImGui::SliderFloat("Voxel Scale", &voxel_scale, 0.00001f, .01f, "%.4f");
            }
        }

        if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text(last_error_message.c_str());
            ImGui::Separator();
            if (ImGui::Button("Close"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::End();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		SDL_GL_SwapWindow(window);
	}

	// TODO: Cleanup
    return EXIT_SUCCESS;
}