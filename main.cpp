#ifdef WIN32
#include <SDL.h>
#undef main
#else
#include <SDL2/SDL.h>
#endif

#include <GL/glew.h>

#include <string_view>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <chrono>
#include <vector>
#include <random>
#include <map>
#include <cmath>
#include <filesystem>

#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/scalar_constants.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/string_cast.hpp>

#include "stb_image.h"

std::string to_string(std::string_view str)
{
    return std::string(str.begin(), str.end());
}

void sdl2_fail(std::string_view message)
{
    throw std::runtime_error(to_string(message) + SDL_GetError());
}

void glew_fail(std::string_view message, GLenum error)
{
    throw std::runtime_error(to_string(message) + reinterpret_cast<const char *>(glewGetErrorString(error)));
}

const char floor_vertex_shader_source[] =
R"(#version 330 core

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

layout (location = 0) in vec3 in_position;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in vec2 in_texcoord;

out vec3 position;
out vec3 normal;
out vec2 texcoord;

void main()
{
    gl_Position = projection * view * model * vec4(in_position, 1.0);
    position = (model * vec4(in_position, 1.0)).xyz;
    texcoord = in_texcoord;
    normal = in_normal;
}
)";

const char floor_fragment_shader_source[] =
R"(#version 330 core

uniform vec3 camera_position;
uniform vec3 ambient_light;

uniform vec3 sun_light;
uniform vec3 sun_direction;

uniform float glossiness;
uniform float roughness;

uniform sampler2D tex;
uniform sampler2D caustics_tex;

in vec3 position;
in vec3 normal;
in vec2 texcoord;

layout (location = 0) out vec4 out_color;

float diffuse(vec3 direction) {
    return max(0.0, dot(normal, direction));
}

vec3 reflect(vec3 direction) {
    float cosine = dot(normal, direction);
    return 2.0 * normal * cosine - direction;
}

float specular(vec3 direction) {
    vec3 view_direction = normalize(camera_position - position);
    vec3 reflected = reflect(direction);
    float power = 1 / (roughness * roughness) - 1;
    return glossiness * pow(max(0.0, dot(reflected, view_direction)), power);
}

void main()
{
    vec2 caustics_texcoord = vec2(position.x / 40.0, position.z / 8.0);
    vec4 caustics_data = texture(caustics_tex, caustics_texcoord);
    vec3 albedo = texture(tex, texcoord).xyz + caustics_data.w * caustics_data.xyz;
    // albedo = caustics_data.xyz;
    vec3 color = albedo * ambient_light;
    float sun_impact = diffuse(sun_direction) + specular(sun_direction);
    color += albedo * sun_impact * sun_light;
    out_color = vec4(color, 1.0);
}
)";


const char env_vertex_shader_source[] =
R"(#version 330 core

layout (location = 0) in vec3 in_position;

uniform mat4 model;
uniform mat4 view;

out vec3 position;

void main()
{
    gl_Position = view * model * vec4(in_position, 1.0);
    gl_Position.z = gl_Position.w;
    position = in_position;
}
)";

const char env_fragment_shader_source[] =
R"(#version 330 core

uniform samplerCube tex;

in vec3 position;

layout (location = 0) out vec4 out_color;

void main()
{
    vec3 color = texture(tex, position).rgb;
    out_color = vec4(color, 1.0);
}
)";

const char water_vertex_shader_source[] =
R"(#version 330 core

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform float time;

layout (location = 0) in vec2 in_position;

out vec3 position;
out vec3 normal;

float get_height() {
    float base_height = 5;
    float add = 0.5 * sin(in_position.x + time) + 0.2 * cos(in_position.y + 3 * time) + 0.1 * sin(in_position.x + 2 * in_position.y + time);
    return base_height + add;
}

float dhdx() {
    return 0.5 * cos(in_position.x + time) + 0.1 * cos(in_position.x + 2 * in_position.y + time);
}

float dhdy() {
    return -0.2 * sin(in_position.y + 3 * time) + 0.2 * cos(in_position.x + 2 * in_position.y + time);
}

void main()
{
    position = vec3(in_position.x, get_height(), in_position.y);
    gl_Position = projection * view * model * vec4(position, 1.0);
    position = (model * vec4(position, 1.0)).xyz;
    normal = normalize(vec3(-dhdx(), 1.0, -dhdy()));
}
)";

const char water_fragment_shader_source[] =
R"(#version 330 core

uniform vec3 camera_position;
uniform vec3 ambient_light;

uniform vec3 sun_light;
uniform vec3 sun_direction;

uniform float glossiness;
uniform float roughness;

uniform samplerCube tex;
uniform sampler2D floor_tex;
uniform sampler2D caustics_tex;

uniform float floor_width;
uniform float floor_height;

in vec3 position;
in vec3 normal;

layout (location = 0) out vec4 out_color;

float diffuse(vec3 direction) {
    return max(0.0, dot(vec3(0.0, 1.0, 0.0), direction));
}

vec3 reflect(vec3 direction) {
    float cosine = dot(normal, direction);
    return 2.0 * normal * cosine - direction;
}

vec3 get_floor(vec3 pos) { 
    vec4 caustics_data = texture(caustics_tex, vec2(pos.x / 40.0, pos.z / 8.0));
    vec3 albedo = texture(floor_tex, vec2(pos.x / 4.0, pos.z / 4.0)).xyz;
    albedo += caustics_data.w * caustics_data.xyz;
    vec3 color = albedo * ambient_light;
    float sun_impact = diffuse(sun_direction);
    color += albedo * sun_impact * sun_light;
    return color;
}

vec3 get_refract(vec3 direction, float n1, float n2) {
    float cosine = dot(normalize(normal), direction);
    float sine = sqrt(1 - cosine * cosine);
    float refract_sine = n1 * sine / n2;
    float refract_cosine = sqrt(1 - refract_sine * refract_sine);
    float h = position.y;
    float straight_floor_x = -direction.x * h / direction.y + position.x;
    float straight_floor_z = -direction.z * h / direction.y + position.z;
    vec3 projection_position = vec3(position.x, 0.0, position.y);
    vec3 straight_projection = vec3(straight_floor_x, 0.0, straight_floor_z) - projection_position;
    vec3 refracted_projection = straight_projection * n1 / n2 * cosine / refract_cosine;
    vec3 refracted_position = projection_position + refracted_projection;
    if (refracted_position.x > 0 && refracted_position.z > 0 && refracted_position.x < floor_width && refracted_position.z < floor_height) {
        return get_floor(refracted_position);
    }
    vec3 refracted_ray = normalize(refracted_position - position);
    return texture(tex, refracted_ray).rgb;
}

void main()
{
    vec3 view_direction = normalize(camera_position - position);
    float n1 = 1.0;
    float n2 = 1.333;
    float cosine = dot(normalize(normal), sun_direction);
    float coef = (n1 - n2) / (n1 + n2);
    coef = coef * coef;
    coef = coef + (1 - coef) * pow(1 - cosine, 5);
    vec3 reflect_color = coef * texture(tex, reflect(view_direction)).rgb;
    vec3 refract_color = (1 - coef) * get_refract(view_direction, n1, n2);
    vec3 color = reflect_color + refract_color;
    out_color = vec4(color, 1.0);
    // out_color = vec4(vec3(1 - cosine), 1.0);
}
)";

const char caustic_vertex_shader_source[] =
R"(#version 330 core

uniform mat4 model;
uniform float time;
uniform vec3 sun_direction;

layout (location = 0) in vec2 in_position;

float get_height() {
    float base_height = 5;
    float add = 0.5 * sin(in_position.x + time) + 0.2 * cos(in_position.y + 3 * time) + 0.1 * sin(in_position.x + 2 * in_position.y + time);
    return base_height + add;
}

float dhdx() {
    return 0.5 * cos(in_position.x + time) + 0.1 * cos(in_position.x + 2 * in_position.y + time);
}

float dhdy() {
    return -0.2 * sin(in_position.y + 3 * time) + 0.2 * cos(in_position.x + 2 * in_position.y + time);
}

vec3 get_refract(vec3 direction, float n1, float n2, vec3 normal, vec3 position) {
    float cosine = dot(normalize(normal), direction);
    float sine = sqrt(1 - cosine * cosine);
    float refract_sine = n1 * sine / n2;
    float refract_cosine = sqrt(1 - refract_sine * refract_sine);
    float h = position.y;
    float straight_floor_x = -direction.x * h / direction.y + position.x;
    float straight_floor_z = -direction.z * h / direction.y + position.z;
    vec3 projection_position = vec3(position.x, 0.0, position.y);
    vec3 straight_projection = vec3(straight_floor_x, 0.0, straight_floor_z) - projection_position;
    vec3 refracted_projection = straight_projection * n1 / n2 * cosine / refract_cosine;
    vec3 refracted_position = projection_position + refracted_projection;
    return refracted_position;
}

void main()
{
    vec3 position = vec3(in_position.x, get_height(), in_position.y);
    position = (model * vec4(position, 1.0)).xyz;
    vec3 normal = normalize(vec3(-dhdx(), 1.0, -dhdy()));
    vec2 texcoord = get_refract(sun_direction, 1.0, 1.33, normal, position).xz;
    texcoord.x /= 40.0;
    texcoord.y /= 8.0;
    gl_Position = vec4(texcoord * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char caustic_fragment_shader_source[] =
R"(#version 330 core

uniform vec3 sun_light;
uniform vec3 sun_direction;

in vec3 normal;

layout (location = 0) out vec4 out_color;

void main()
{
    float n1 = 1.0;
    float n2 = 1.333;
    float cosine = dot(normalize(normal), sun_direction);
    float coef = (n1 - n2) / (n1 + n2);
    coef = coef * coef;
    coef = coef + (1 - coef) * pow(1 - cosine, 5);
    vec3 color = (1 - coef) * sun_light;
    out_color = vec4(sun_light, 1.0 - coef);
}
)";


GLuint create_shader(GLenum type, const char * source)
{
    GLuint result = glCreateShader(type);
    glShaderSource(result, 1, &source, nullptr);
    glCompileShader(result);
    GLint status;
    glGetShaderiv(result, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE)
    {
        GLint info_log_length;
        glGetShaderiv(result, GL_INFO_LOG_LENGTH, &info_log_length);
        std::string info_log(info_log_length, '\0');
        glGetShaderInfoLog(result, info_log.size(), nullptr, info_log.data());
        throw std::runtime_error("Shader compilation failed: " + info_log);
    }
    return result;
}

template <typename ... Shaders>
GLuint create_program(Shaders ... shaders)
{
    GLuint result = glCreateProgram();
    (glAttachShader(result, shaders), ...);
    glLinkProgram(result);

    GLint status;
    glGetProgramiv(result, GL_LINK_STATUS, &status);
    if (status != GL_TRUE)
    {
        GLint info_log_length;
        glGetProgramiv(result, GL_INFO_LOG_LENGTH, &info_log_length);
        std::string info_log(info_log_length, '\0');
        glGetProgramInfoLog(result, info_log.size(), nullptr, info_log.data());
        throw std::runtime_error("Program linkage failed: " + info_log);
    }

    return result;
}

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 texcoord;
};

glm::vec2 get_water_position(int i, int j, float floor_width, float floor_height, int width_water_cnt, int height_water_cnt) {
    // std::cout << floor_width / float(width_water_cnt) * i << ' ' << floor_height / float(height_water_cnt) * j << std::endl;
    return {floor_width / float(width_water_cnt) * i, floor_height / float(height_water_cnt) * j};
}

int main() try
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        sdl2_fail("SDL_Init: ");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window * window = SDL_CreateWindow("Water pool",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        800, 600,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);

    if (!window)
        sdl2_fail("SDL_CreateWindow: ");

    int width, height;
    SDL_GetWindowSize(window, &width, &height);

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context)
        sdl2_fail("SDL_GL_CreateContext: ");

    if (auto result = glewInit(); result != GLEW_NO_ERROR)
        glew_fail("glewInit: ", result);

    if (!GLEW_VERSION_3_3)
        throw std::runtime_error("OpenGL 3.3 is not supported");

    auto caustics_vertex_shader = create_shader(GL_VERTEX_SHADER, caustic_vertex_shader_source);
    auto caustics_fragment_shader = create_shader(GL_FRAGMENT_SHADER, caustic_fragment_shader_source);
    auto caustics_program = create_program(caustics_vertex_shader, caustics_fragment_shader);

    GLuint caustics_model_location = glGetUniformLocation(caustics_program, "model");
    GLuint caustics_time_location = glGetUniformLocation(caustics_program, "time");
    GLuint caustics_sun_direction_location = glGetUniformLocation(caustics_program, "sun_direction");
    GLuint caustics_sun_color_location = glGetUniformLocation(caustics_program, "sun_light");

    auto water_vertex_shader = create_shader(GL_VERTEX_SHADER, water_vertex_shader_source);
    auto water_fragment_shader = create_shader(GL_FRAGMENT_SHADER, water_fragment_shader_source);
    auto water_program = create_program(water_vertex_shader, water_fragment_shader);

    GLuint water_model_location = glGetUniformLocation(water_program, "model");
    GLuint water_view_location = glGetUniformLocation(water_program, "view");
    GLuint water_projection_location = glGetUniformLocation(water_program, "projection");
    GLuint water_camera_position_location = glGetUniformLocation(water_program, "camera_position");
    GLuint water_sun_direction_location = glGetUniformLocation(water_program, "sun_direction");
    GLuint water_sun_color_location = glGetUniformLocation(water_program, "sun_light");
    GLuint water_ambient_color_location = glGetUniformLocation(water_program, "ambient_light");
    GLuint water_glossiness_location = glGetUniformLocation(water_program, "glossiness");
    GLuint water_roughness_location = glGetUniformLocation(water_program, "roughness");
    GLuint water_time_location = glGetUniformLocation(water_program, "time");
    GLuint water_env_texture_location = glGetUniformLocation(water_program, "tex");
    GLuint water_caustics_texture_location = glGetUniformLocation(water_program, "caustics_tex");
    GLuint water_floor_texture_location = glGetUniformLocation(water_program, "floor_tex");
    GLuint water_floor_width_location = glGetUniformLocation(water_program, "floor_width");
    GLuint water_floor_height_location = glGetUniformLocation(water_program, "floor_height");

    auto env_vertex_shader = create_shader(GL_VERTEX_SHADER, env_vertex_shader_source);
    auto env_fragment_shader = create_shader(GL_FRAGMENT_SHADER, env_fragment_shader_source);
    auto env_program = create_program(env_vertex_shader, env_fragment_shader);

    GLuint env_texture_location = glGetUniformLocation(env_program, "tex");
    GLuint env_model_location = glGetUniformLocation(env_program, "model");
    GLuint env_view_location = glGetUniformLocation(env_program, "view");

    auto floor_vertex_shader = create_shader(GL_VERTEX_SHADER, floor_vertex_shader_source);
    auto floor_fragment_shader = create_shader(GL_FRAGMENT_SHADER, floor_fragment_shader_source);
    auto floor_program = create_program(floor_vertex_shader, floor_fragment_shader);

    GLuint floor_model_location = glGetUniformLocation(floor_program, "model");
    GLuint floor_view_location = glGetUniformLocation(floor_program, "view");
    GLuint floor_projection_location = glGetUniformLocation(floor_program, "projection");
    GLuint floor_camera_position_location = glGetUniformLocation(floor_program, "camera_position");
    GLuint floor_sun_direction_location = glGetUniformLocation(floor_program, "sun_direction");
    GLuint floor_sun_color_location = glGetUniformLocation(floor_program, "sun_light");
    GLuint floor_ambient_color_location = glGetUniformLocation(floor_program, "ambient_light");
    GLuint floor_glossiness_location = glGetUniformLocation(floor_program, "glossiness");
    GLuint floor_roughness_location = glGetUniformLocation(floor_program, "roughness");
    GLuint floor_texture_location = glGetUniformLocation(floor_program, "tex");
    GLuint floor_caustics_texture_location = glGetUniformLocation(floor_program, "caustics_tex");
    glUseProgram(floor_program);

    const std::string project_root = PROJECT_ROOT;
    std::string floor_texture_path = project_root + "/floor.png";

    GLuint floor_vao, floor_vbo;
    glGenVertexArrays(1, &floor_vao);
    glBindVertexArray(floor_vao);

    const float floor_width = 40;
    const float floor_height = 8;
    glm::vec3 floor_normal = {0, 1, 0};
    std::vector<Vertex> floor_data = {{{0, 0, 0}, floor_normal, {0, 0}}, {{0, 0, floor_height}, floor_normal, {0, floor_height / 4.0}},
                                        {{floor_width, 0, 0}, floor_normal, {floor_width / 4.0, 0}}, {{floor_width, 0, 0}, floor_normal, {floor_width / 4.0, 0}}, 
                                        {{0, 0, floor_height}, floor_normal, {0, floor_height / 4.0}}, {{floor_width, 0, floor_height}, floor_normal, {floor_width / 4.0, floor_height / 4.0}}};

    glGenBuffers(1, &floor_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, floor_vbo);
    glBufferData(GL_ARRAY_BUFFER, floor_data.size() * sizeof(Vertex), floor_data.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(12));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(24));

    GLuint water_vao, water_vbo;
    glGenVertexArrays(1, &water_vao);
    glBindVertexArray(water_vao);

    const int width_water_cnt = 500;
    const int height_water_cnt = 100;
    std::vector<glm::vec2> water_points;
    for (int i = 0; i < width_water_cnt; ++i) {
        for (int j = 0; j < height_water_cnt; ++j) {
            water_points.push_back(get_water_position(i, j, floor_width, floor_height, width_water_cnt, height_water_cnt));
            water_points.push_back(get_water_position(i, j + 1, floor_width, floor_height, width_water_cnt, height_water_cnt));
            water_points.push_back(get_water_position(i + 1, j, floor_width, floor_height, width_water_cnt, height_water_cnt));
            water_points.push_back(get_water_position(i + 1, j, floor_width, floor_height, width_water_cnt, height_water_cnt));
            water_points.push_back(get_water_position(i, j + 1, floor_width, floor_height, width_water_cnt, height_water_cnt));
            water_points.push_back(get_water_position(i + 1, j + 1, floor_width, floor_height, width_water_cnt, height_water_cnt));
        }
    }

    glGenBuffers(1, &water_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, water_vbo);
    glBufferData(GL_ARRAY_BUFFER, water_points.size() * sizeof(glm::vec2), water_points.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), (void*)(0));

    GLuint tex;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    int x, y, n;
    unsigned char* pixels_data = stbi_load(floor_texture_path.c_str(), &x, &y, &n, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, x, y, 0, GL_RGBA, GL_UNSIGNED_BYTE, (void*)pixels_data);
    stbi_image_free(pixels_data);

    GLuint env_vao, env_vbo;
    glGenVertexArrays(1, &env_vao);
    glBindVertexArray(env_vao);

    std::vector<glm::vec3> env_data = {{-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1},
                                         {-1, -1, -1}, {-1, -1, 1}, {-1, 1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1},
                                         {-1, -1, -1}, {-1, 1, -1}, {1, -1, -1}, {1, -1, -1}, {-1, 1, -1}, {1, 1, -1},
                                         {1, -1, -1}, {1, 1, -1}, {1, -1, 1}, {1, -1, 1}, {1, 1, -1}, {1, 1, 1},
                                         {-1, -1, -1}, {1, -1, -1}, {-1, -1, 1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1},
                                         {-1, 1, -1}, {-1, 1, 1}, {1, 1, -1}, {1, 1, -1}, {-1, 1, 1}, {1, 1, 1}
                                      };
    for (int i = 0; i < env_data.size(); ++i) {
        env_data[i] *= glm::vec3(2);
    }
    glGenBuffers(1, &env_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, env_vbo);
    glBufferData(GL_ARRAY_BUFFER, env_data.size() * sizeof(glm::vec3), env_data.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)(0));

    GLuint env_tex;
    glGenTextures(1, &env_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, env_tex);
    const std::string env_path = project_root + "/environment/";
    std::string env_names[6] = {"posx.jpg", "negx.jpg", "posy.jpg", "negy.jpg", "posz.jpg", "negz.jpg"};
    for (int i = 0; i < 6; ++i) {
        unsigned char* env_data = stbi_load((env_path + env_names[i]).c_str(), &x, &y, &n, 4);
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA8, x, y, 0, GL_RGBA, GL_UNSIGNED_BYTE, (void*)env_data);
        stbi_image_free(env_data);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE); 


    const int caustics_resolution = 512;
    GLuint caustics_tex, caustics_fbo, caustics_rbf;
    glGenTextures(1, &caustics_tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, caustics_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, caustics_resolution, caustics_resolution, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &caustics_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, caustics_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, caustics_tex, 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cout << "Incomplete buffer" << std::endl;
    }

    auto last_frame_start = std::chrono::high_resolution_clock::now();

    float time = 0.f;

    std::map<SDL_Keycode, bool> button_down;

    float view_angle = 0.f;
    float camera_distance = 1.5f;

    float camera_rotation = 0.f;
    float camera_height = 1.f;

    glm::vec3 camera_position = glm::vec3(floor_width / 2.0, 10.f, 20.f);
    glm::vec3 camera_front = glm::vec3(0.f, 0.f, -1.f);
    glm::vec3 base_camera_front = glm::vec3(0.f, 0.f, -1.f);
    glm::vec3 camera_up = glm::vec3(0.f, 1.f, 0.f);

    bool paused = false;

    bool running = true;
    while (running)
    {
        for (SDL_Event event; SDL_PollEvent(&event);) switch (event.type)
        {
        case SDL_QUIT:
            running = false;
            break;
        case SDL_WINDOWEVENT: switch (event.window.event)
            {
            case SDL_WINDOWEVENT_RESIZED:
                width = event.window.data1;
                height = event.window.data2;
                glViewport(0, 0, width, height);
                break;
            }
            break;
        case SDL_KEYDOWN:
            button_down[event.key.keysym.sym] = true;
            if (event.key.keysym.sym == SDLK_p)
                paused = !paused;
            break;
        case SDL_KEYUP:
            button_down[event.key.keysym.sym] = false;
            break;
        }

        if (!running)
            break;

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration_cast<std::chrono::duration<float>>(now - last_frame_start).count();
        last_frame_start = now;

        if (!paused) {
            time += dt;
        }
        if (button_down[SDLK_w])
            camera_position += 6 * dt * camera_front;
        if (button_down[SDLK_s])
            camera_position -= 6 * dt * camera_front;
        if (button_down[SDLK_a])
            camera_position -= 6 * dt * glm::normalize(glm::cross(camera_front, camera_up));
        if (button_down[SDLK_d])
            camera_position += 6 * dt * glm::normalize(glm::cross(camera_front, camera_up));
        if (button_down[SDLK_LCTRL])
            camera_position -= 6 * dt * camera_up;
        if (button_down[SDLK_SPACE])
            camera_position += 6 * dt * camera_up;

        if (button_down[SDLK_LEFT])
            camera_rotation -= 2.f * dt;
        if (button_down[SDLK_RIGHT])
            camera_rotation += 2.f * dt;

        if (button_down[SDLK_UP])
            view_angle -= 2.f * dt;
        if (button_down[SDLK_DOWN])
            view_angle += 2.f * dt;


        float near = 0.01f;
        float far = 100.0;
        float aspect_ratio = width / float(height);

        glm::mat4 model = glm::mat4(1.f);

        glm::mat4 rotation_matrix(1.f);
        rotation_matrix = glm::rotate(rotation_matrix, view_angle, {1.f, 0.f, 0.f});
        rotation_matrix = glm::rotate(rotation_matrix, camera_rotation, {0.f, 1.f, 0.f});
        camera_front = base_camera_front * glm::mat3(rotation_matrix);

        glm::mat4 view(1.f);
        view = glm::lookAt(camera_position, camera_position + camera_front, camera_up);

        glm::mat4 projection = glm::perspective(glm::pi<float>() / 2.f, (1.f * width) / height, near, far);

        glm::vec3 light_direction = glm::normalize(glm::vec3(0.9, 1.f, -0.2));
        glm::vec3 sun_color = glm::vec3(1.0, 0.9, 0.8);

        // Caustics

        glUseProgram(caustics_program);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, caustics_fbo);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, caustics_resolution, caustics_resolution);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);

        glUniformMatrix4fv(caustics_model_location, 1, GL_FALSE, reinterpret_cast<float *>(&model));
        glUniform1f(caustics_time_location, time);
        glUniform3fv(caustics_sun_direction_location, 1, reinterpret_cast<float *>(&light_direction));
        glUniform3f(caustics_sun_color_location, sun_color.x, sun_color.y, sun_color.z);

        glBindVertexArray(water_vao);
        glBindBuffer(GL_ARRAY_BUFFER, water_vbo);

        glDrawArrays(GL_TRIANGLES, 0, water_points.size());

        // Environment
        glUseProgram(env_program);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glClearColor(0.8, 0.8, 1.f, 0.f);
        glViewport(0, 0, width, height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glUniform1i(env_texture_location, 1);
        glUniformMatrix4fv(env_model_location, 1, GL_FALSE, reinterpret_cast<float *>(&model));
        
        glm::mat4 env_rotation_matrix(1.f);
        env_rotation_matrix = glm::rotate(env_rotation_matrix, -view_angle, {1.f, 0.f, 0.f});
        env_rotation_matrix = glm::rotate(env_rotation_matrix, -camera_rotation, {0.f, 1.f, 0.f});
        glm::vec3 env_camera_front = base_camera_front * glm::mat3(env_rotation_matrix);
        glm::mat4 env_view(1.f);
        env_view = glm::lookAt(glm::vec3(0), env_camera_front, camera_up);
        
        glUniformMatrix4fv(env_view_location, 1, GL_FALSE, reinterpret_cast<float *>(&env_view));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_CUBE_MAP, env_tex);
        glBindVertexArray(env_vao);
        glBindBuffer(GL_ARRAY_BUFFER, env_vbo);

        glDrawArrays(GL_TRIANGLES, 0, 36);

        // Floor
        glUseProgram(floor_program);
        glEnable(GL_DEPTH_TEST);

        glUniformMatrix4fv(floor_model_location, 1, GL_FALSE, reinterpret_cast<float *>(&model));
        glUniformMatrix4fv(floor_projection_location, 1, GL_FALSE, reinterpret_cast<float *>(&projection));
        glUniformMatrix4fv(floor_view_location, 1, GL_FALSE, reinterpret_cast<float *>(&view));
        glUniform3fv(floor_sun_direction_location, 1, reinterpret_cast<float *>(&light_direction));
        glUniform3fv(floor_camera_position_location, 1, reinterpret_cast<float *>(&camera_position));
        glUniform1i(floor_texture_location, 0);
        glUniform1i(floor_caustics_texture_location, 2);
        glUniform3f(floor_ambient_color_location, 0.2, 0.2, 0.2);
        glUniform3f(floor_sun_color_location, sun_color.x, sun_color.y, sun_color.z);
        glUniform1f(floor_glossiness_location, 3.0);
        glUniform1f(floor_roughness_location, 0.05);

        glBindVertexArray(floor_vao);
        glBindBuffer(GL_ARRAY_BUFFER, floor_vbo);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, caustics_tex);

        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Water
        glUseProgram(water_program);
        glEnable(GL_DEPTH_TEST);

        glUniformMatrix4fv(water_model_location, 1, GL_FALSE, reinterpret_cast<float *>(&model));
        glUniformMatrix4fv(water_projection_location, 1, GL_FALSE, reinterpret_cast<float *>(&projection));
        glUniformMatrix4fv(water_view_location, 1, GL_FALSE, reinterpret_cast<float *>(&view));
        glUniform3fv(water_sun_direction_location, 1, reinterpret_cast<float *>(&light_direction));
        glUniform3fv(water_camera_position_location, 1, reinterpret_cast<float *>(&camera_position));
        glUniform1f(water_time_location, time);
        glUniform3f(water_ambient_color_location, 0.2, 0.2, 0.2);
        glUniform3f(water_sun_color_location, sun_color.x, sun_color.y, sun_color.z);
        glUniform1f(water_glossiness_location, 3.0);
        glUniform1f(water_roughness_location, 0.05);
        glUniform1i(water_env_texture_location, 1);
        glUniform1i(water_floor_texture_location, 0);
        glUniform1i(water_caustics_texture_location, 2);
        glUniform1f(water_floor_width_location, floor_width);
        glUniform1f(water_floor_height_location, floor_height);

        glBindVertexArray(water_vao);
        glBindBuffer(GL_ARRAY_BUFFER, water_vbo);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_CUBE_MAP, env_tex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, caustics_tex);

        glDrawArrays(GL_TRIANGLES, 0, water_points.size());

        SDL_GL_SwapWindow(window);
    }

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
}
catch (std::exception const & e)
{
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
}
