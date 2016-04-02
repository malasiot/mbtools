#include "mesh_tile_renderer.hpp"
#include "mesh.hpp"
#include "mesh_tile.pb.h"

#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/io/gzip_stream.h>

#include <png.h>
#include <fstream>
#include <iostream>

#define GL_GLEXT_PROTOTYPES
#include <GL/glew.h>

#include <GLFW/glfw3.h>

using namespace std ;

struct DataBuffers {

    struct Channel {
        string name_ ;
        uint32_t dim_, offset_ ;
    };

    vector<Channel> channels_ ;
    vector<float> coords_, attr_ ;
    vector<GLuint> indices_ ;
};

// decode protobuf object and serialize data tp buffers

bool decode( const string &data, DataBuffers &buf ) {

    mesh_tile::Tile tile_msg_ ;

    if ( data[0] != '\x1F' && data[1] != '\x8B' ) {
        if ( !tile_msg_.ParseFromString(data) )
            return false ;
    } else { // compressed
        ::google::protobuf::io::ArrayInputStream compressedStream(data.data(), data.size());
        ::google::protobuf::io::GzipInputStream decompressingStream(&compressedStream);
        if ( !tile_msg_.ParsePartialFromZeroCopyStream(&decompressingStream) )
            return false ;
    }

    // coordinates

    const mesh_tile::Tile_Mesh &mesh = tile_msg_.mesh() ;

    std::copy(mesh.coords().begin(), mesh.coords().end(), std::back_inserter(buf.coords_)) ;
    std::copy(mesh.triangles().begin(), mesh.triangles().end(), std::back_inserter(buf.indices_)) ;

    uint64_t offset = 0 ;

    for(uint i=0 ; i<tile_msg_.channels_size() ; i++ )
    {
        const mesh_tile::Tile_Channel &channel = tile_msg_.channels(i) ;
        DataBuffers::Channel tc ;
        tc.name_ = channel.name() ;
        tc.dim_ = channel.dimensions() ;
        tc.offset_ = offset ;
        uint64_t ds = channel.data_size() ;
        offset += ds ;
        std::copy(channel.data().begin(), channel.data().end(), std::back_inserter(buf.attr_)) ;
        buf.channels_.push_back(std::move(tc)) ;
    }

    return true ;
}


static void png_write_callback(png_structp  png_ptr, png_bytep data, png_size_t length) {
    std::vector<uint8_t> *p = (std::vector<uint8_t>*)png_get_io_ptr(png_ptr);
    p->insert(p->end(), data, data + length);
}

static bool save_png(uint8_t *pixels, int w, int h, vector<uint8_t> &data)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);

    if ( !png ) return false;

    png_infop info = png_create_info_struct(png);

    if (!info) {
        png_destroy_write_struct(&png, &info);
        return false;
    }

    png_set_IHDR(png, info, w, h, 8 /* depth */, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_colorp palette = (png_colorp)png_malloc(png, PNG_MAX_PALETTE_LENGTH * sizeof(png_color));

    if (!palette) {
        png_destroy_write_struct(&png, &info);
        return false;
    }

    png_set_PLTE(png, info, palette, PNG_MAX_PALETTE_LENGTH);
    png_set_packing(png);

    png_bytepp rows = (png_bytepp)png_malloc(png, h * sizeof(png_bytep));

    for (int i = 0; i < h; ++i)
        rows[i] = (png_bytep)(pixels + (h - i) * w * 4);

    png_set_rows(png, info, &rows[0]);
    png_set_write_fn(png, (png_voidp)&data, png_write_callback, NULL);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);
    png_free(png, palette);
    png_destroy_write_struct(&png, &info);

    delete[] rows;

    return true ;
}
/******************************************************************************************/

struct ShaderProgram {
    const char *name_ ;
    const char *vertex_shader_ ;
    const char *fragment_shader_ ;
};

vector<ShaderProgram> g_programs = {
  { "hillshade",
    R"(#version 330

    uniform vec2 offset ;
    uniform float scale ;

    layout (location = 0) in vec2 coords;
    layout (location = 2) in vec3 normals;

    out vec3 Normal ;

    void main()
    {
        vec2 cp = 2*(coords - offset)/scale - vec2(1, 1);
        gl_Position = vec4(cp.xy, 0, 1) ;
        Normal = normals ;
    })",
    R"(
    #version 330

    in vec3 Normal0;
    out vec4 frag_color;

    uniform vec3 ldir ;

    void main()
    {
        float g = dot(ldir, Normal0) ;
        frag_color = vec4(g, g, g, 1) ;
    };
    )"
    }
};

class RenderingContext {
public:

    RenderingContext(uint32_t ts): ts_(ts) {}

    bool init(const string &) ;
    void release() ;
    void init_buffers(const DataBuffers &data, const BBox &box) ;
    void render(const std::string &name);
    void render_debug(const std::string &name);
    void clear();
private:

    static GLuint create_shader(const char *shader_source, GLuint stype) ;
    static bool link_program(GLuint prog) ;

    bool init_shaders(const vector<ShaderProgram> &shaders) ;

    friend class MeshTileRenderer ;

    GLFWwindow* win_ = 0;
    GLuint fbo_ = 0, texture_id_ = 0;
    uint32_t ts_ ;
    map<string, GLuint> programs_ ;
    GLuint vao_, coords_, attr_, indices_, tf_ ;
    GLuint elem_count_ ;
    vector<float> tfbuf_ ;
};

bool RenderingContext::init(const string &cfg) {

    if( !glfwInit() ) return false ;

    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);

    glfwWindowHint(GLFW_VISIBLE, GL_FALSE);
    win_ = glfwCreateWindow(ts_, ts_, "tile", 0, 0);

    if ( !win_ )     {
        glfwTerminate();
        return false ;
    }

    glfwMakeContextCurrent(win_);

    if( glewInit() != GLEW_OK ) return false ;

    // create a framebuffer object
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    // create a texture object
    glGenTextures(1, &texture_id_);
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ts_, ts_, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id_, 0);

    // bind buffers
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    init_shaders(g_programs) ;

    return true ;
}

void RenderingContext::release() {
    if ( texture_id_ )
        glDeleteTextures(1, &texture_id_ );

    texture_id_ = 0;

    // clean up FBO
    if ( fbo_ ) glDeleteFramebuffers(1, &fbo_);
    fbo_ = 0;

    glfwDestroyWindow(win_);

    glfwTerminate();
}


GLuint RenderingContext::create_shader(const char *shader_source, GLuint stype)
{
    GLuint shader_obj = glCreateShader(stype);

    if ( shader_obj == 0) return 0 ;

    const GLchar* p[1];
    p[0] = shader_source ;
    GLint lengths[1] = { (GLint)strlen(shader_source) };

    glShaderSource(shader_obj, 1, p, lengths);

    glCompileShader(shader_obj);

    GLint success;
    glGetShaderiv(shader_obj, GL_COMPILE_STATUS, &success);

    if (!success) {
        GLchar InfoLog[1024];
        glGetShaderInfoLog(shader_obj, 1024, NULL, InfoLog);
        fprintf(stderr, "Error compiling shader: '%s'\n", InfoLog);
        return 0 ;
    }

    return shader_obj ;
}

bool RenderingContext::link_program(GLuint prog)
{
    GLchar error_log[1024] = { 0 };

    glLinkProgram(prog) ;

    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);

    if (success == 0) {
        glGetProgramInfoLog(prog, sizeof(error_log), NULL, error_log);
        fprintf(stderr, "Error linking shader program: '%s'\n", error_log);
        return false ;
    }

    glValidateProgram(prog);
    glGetProgramiv(prog, GL_VALIDATE_STATUS, &success);

    if (!success) {
        glGetProgramInfoLog(prog, sizeof(error_log), NULL, error_log);
        fprintf(stderr, "Invalid shader program: '%s'\n", error_log);
        return false ;
    }

    return true ;
}


bool RenderingContext::init_shaders(const vector<ShaderProgram> &shaders)
{
    for( const ShaderProgram &s: shaders ) {

        GLuint pid = glCreateProgram(); ;
        programs_[s.name_] = pid ;

        if ( s.vertex_shader_ ) {
            GLuint sid = create_shader(s.vertex_shader_, GL_VERTEX_SHADER);
            if ( sid == 0 ) return false ;

            glAttachShader(pid, sid);
        }

        if ( s.fragment_shader_ ) {
            GLuint sid = create_shader(s.fragment_shader_, GL_FRAGMENT_SHADER);
            if ( sid == 0 ) return false ;

            glAttachShader(pid, sid);
        }

        const char* varyings[2] = { "Normal" };
        glTransformFeedbackVaryings(pid, 1, varyings, GL_INTERLEAVED_ATTRIBS);

        if ( !link_program(pid) )
            return false ;


    }

    return true ;
}

#define POSITION_LOCATION    0
#define ATTRIBUTE_LOCATION   1

void RenderingContext::init_buffers(const DataBuffers &buf, const BBox &box) {

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    glGenBuffers(1, &coords_);
    glBindBuffer(GL_ARRAY_BUFFER, coords_);
    glBufferData(GL_ARRAY_BUFFER, buf.coords_.size() * sizeof(GLfloat), buf.coords_.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(POSITION_LOCATION);
    glVertexAttribPointer(POSITION_LOCATION, 2, GL_FLOAT, GL_FALSE, 0, NULL);

    glGenBuffers(1, &indices_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, buf.indices_.size() * sizeof(GLuint), buf.indices_.data(), GL_STATIC_DRAW);

    elem_count_ = buf.indices_.size() ;

    glGenBuffers(1, &attr_);
    glBindBuffer(GL_ARRAY_BUFFER, attr_);
    glBufferData(GL_ARRAY_BUFFER, buf.attr_.size() * sizeof(GLfloat), buf.attr_.data(), GL_STATIC_DRAW);

    uint k=0 ;
    for( const DataBuffers::Channel &c: buf.channels_) {
        GLuint loc = ATTRIBUTE_LOCATION + k++ ;
        glBindBuffer(GL_ARRAY_BUFFER, attr_);
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, c.dim_, GL_FLOAT, GL_FALSE, 0, (GLvoid *)(c.offset_ * sizeof(float)));
    }

    float scale =  box.width() ;
    float ofx = box.minx_ ;
    float ofy = box.miny_  ;

    float theta = M_PI/4 ;
    float phi = M_PI/4 ;

    float gx = cos(phi) * sin(theta) ;
    float gy = sin(phi) * sin(theta) ;
    float gz = cos(theta) ;

    for ( auto pr: programs_ ) {
        glUseProgram(pr.second) ;

        GLint loc = glGetUniformLocation(pr.second, "offset") ;
        if ( loc != -1 ) glUniform2f(loc, ofx, ofy) ;
        loc = glGetUniformLocation(pr.second, "scale") ;
        if ( loc != -1 ) glUniform1f(loc, scale) ;

        loc = glGetUniformLocation(pr.second, "ldir") ;
        if ( loc != -1 ) glUniform3f(loc, gx, gy, gz) ;
    }

}

void RenderingContext::clear() {
    glDeleteVertexArrays(1, &vao_) ;
    glDeleteBuffers(1, &coords_) ;
    glDeleteBuffers(1, &indices_) ;
    glDeleteBuffers(1, &attr_) ;
}

void RenderingContext::render(const std::string &name) {

    GLuint pid = programs_[name] ;

    glUseProgram(pid) ;
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, elem_count_, GL_UNSIGNED_INT, 0) ;
    glBindVertexArray(0) ;
    glUseProgram(0) ;

    glFlush() ;

}

void RenderingContext::render_debug(const std::string &name) {

    GLuint pid = programs_[name] ;

    glUseProgram(pid) ;
    glBindVertexArray(vao_);
    glEnable(GL_RASTERIZER_DISCARD);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tf_);
    glBeginTransformFeedback(GL_TRIANGLES);
    glDrawElements(GL_TRIANGLES, elem_count_, GL_UNSIGNED_INT, 0) ;
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    glBindVertexArray(0) ;
    glUseProgram(0) ;

    glFlush();
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfbuf_.size() * sizeof(float), tfbuf_.data());
}

MeshTileRenderer::MeshTileRenderer(const string &cfg_file, uint32_t ts): tile_size_(ts), ctx_(new RenderingContext(ts))
{
    ctx_->init(cfg_file) ;
}

MeshTileRenderer::~MeshTileRenderer() {
    ctx_->release() ;
}

std::string MeshTileRenderer::render(uint32_t x, uint32_t y, uint32_t z,
        const std::string &bytes, const std::string &prname)
{
//    std::lock_guard<std::mutex> lock(mtx_) ;

    // we need to do this since we are possibly in a different thread

    glfwMakeContextCurrent(ctx_->win_);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    DataBuffers data ;

    if ( decode(bytes, data) ) {

        BBox box ;
        tms::tileBounds(x, y, z, box.minx_, box.miny_, box.maxx_, box.maxy_) ;
        ctx_->init_buffers(data, box) ;

        ctx_->render(prname) ;

        ctx_->clear() ;
    }



    vector<uint8_t> pixels( tile_size_ * tile_size_ * 4 ) ;

    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, tile_size_, tile_size_, GL_RGBA, GL_UNSIGNED_BYTE,  pixels.data());

    vector<uint8_t> out ;
    if ( save_png(pixels.data(), tile_size_, tile_size_, out) )
    {
        string s(out.begin(), out.end()) ;

        {
        ofstream strm("/tmp/oo.png", ios::binary) ;
        strm.write(s.data(), s.size()) ;
        }
        return std::move(s) ;
    }


}
