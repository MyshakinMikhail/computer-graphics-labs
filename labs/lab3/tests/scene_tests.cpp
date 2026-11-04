#include "scene.hpp"
#include "image.hpp"
#include <limits>
#include <filesystem>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
bool close(float a,float b,float eps=1e-4f){return std::abs(a-b)<eps;}
bool finite(glm::mat4 m) {
    for(int c=0;c<4;++c)for(int r=0;r<4;++r)if(!std::isfinite(m[c][r]))return false;
    return true;
}
void geometry() {
    const auto mesh=lab3::octahedron_mesh();require(mesh.vertices.size()==24,"Octahedron must have eight triangles");
    for(size_t i=0;i<mesh.vertices.size();i+=3) {
        const auto a=mesh.vertices[i],b=mesh.vertices[i+1],c=mesh.vertices[i+2];
        require(close(glm::length(a.normal),1),"Normals must be unit vectors");
        require(glm::dot(a.normal,(a.position+b.position+c.position)/3.0f)>0,"Normals must point outwards");
        require(close(glm::dot(a.normal,b.position-a.position),0),"Normal must be perpendicular to the face");
        require(close(glm::length(a.position-b.position),std::sqrt(2.0f)),"Octahedron edges must be equal");
    }
    for(const auto& v:lab3::floor_mesh().vertices)require(v.normal.y>0.99f,"Floor normals must face up");
    // Nonuniform scale: transformed normal must remain perpendicular to transformed edges.
    lab3::Object object;object.scale={0.2f,2,0.7f};object.rotation={12,40,20};
    const auto m=object.model();const auto n=glm::transpose(glm::inverse(glm::mat3(m)));
    const auto& a=mesh.vertices[0];const auto& b=mesh.vertices[1];
    require(close(glm::dot(n*a.normal,glm::mat3(m)*(b.position-a.position)),0),"Normal matrix must handle nonuniform scale");
}
void lighting() {
    require(close(lab3::inverse_square(4)/lab3::inverse_square(16),4),"Double distance must reduce intensity four times");
    require(std::isfinite(lab3::inverse_square(0)),"Source coincidence must not divide by zero");
    const float inner=std::cos(glm::radians(15.0f)),outer=std::cos(glm::radians(30.0f));
    require(close(lab3::cone_weight(1,inner,outer),1),"Cone center must have full intensity");
    require(close(lab3::cone_weight(outer-0.1f,inner,outer),0),"Outside cone must be dark");
    require(close(lab3::cone_weight((inner+outer)/2,inner,outer),0.5f),"Cone penumbra must interpolate");
    require(close(lab3::cone_weight(outer,outer,outer),1),"Equal angles must support hard edge");
    require(close(lab3::cone_weight(outer-0.001f,outer,outer),0),"Hard edge must reject outside cone");
    require(close(glm::length(lab3::safe_direction({0,0,0})),1),"Zero direction must have valid fallback");
}
void camera() {
    lab3::Camera c;c.rotate(1000,1000);require(c.pitch>=-89&&c.pitch<=89,"Pitch must be clamped");
    require(close(glm::length(c.forward()),1),"Camera forward must be normalized");
    require(finite(c.view_projection(16.0f/9)),"Camera must produce finite matrix at pitch limit");
    c=lab3::Camera{};const auto start=c.position;c.move({0,0,1},0.1f,false);
    require(close(glm::length(c.position-start),c.speed*0.1f),"Camera speed must use delta time");
    c.position=start;c.move({1,0,1},0.1f,false);
    require(close(glm::length(c.position-start),c.speed*0.1f),"Diagonal movement must not be faster");
}
void contracts() {
    static_assert(offsetof(lab3::SceneUniform,camera_position)==64);
    static_assert(offsetof(lab3::SceneUniform,light_counts)==128);
    static_assert(offsetof(lab3::ModelUniform,albedo)==128);
    static_assert(offsetof(lab3::SpotGPU,outer)==48);
    lab3::Scene s;s.reset();s.points.clear();s.spots.clear();
    auto u=s.uniform(1);require(u.light_counts.x==0&&u.light_counts.y==0,"Empty light lists must report zero");
    s.points.resize(37);s.spots.resize(41);u=s.uniform(1);
    require(u.light_counts.x==37&&u.light_counts.y==41,"Scene must not hardcode light counts");
    s.points[0].enabled=false;s.spots[0].enabled=false;
    require(s.point_gpu(0).color_intensity.w==0&&s.spot_gpu(0).color_intensity.w==0,"Disabled lights must contribute zero");
    s.spots[0].direction={0,0,0};const auto spot=s.spot_gpu(0);
    require(close(glm::length(glm::vec3(spot.direction_inner)),1),"GPU spot direction must remain normalized");
    require(spot.direction_inner.w>=spot.outer.x,"Cosine ordering must match cone angles");
    s.light_angle=1;const auto p=s.point_position(0);s.animate_lights=false;
    require(glm::length(s.point_position(0)-p)<1e-5f,"Pausing animation must retain light position");
    s.reset();require(s.points.size()==2&&s.spots.size()==1&&s.objects.size()==3,"Reset must restore demo scene");
}
void textures_and_buffers() {
    for(const auto mesh:{lab3::octahedron_mesh(),lab3::cube_mesh(),lab3::floor_mesh()}) {
        require(!mesh.indices.empty() && mesh.indices.size()%3==0,"Indexed triangle list required");
        for(auto i:mesh.indices)require(i<mesh.vertices.size(),"Index must refer to a vertex");
        for(size_t i=0;i<mesh.indices.size();i+=3) {
            const auto& a=mesh.vertices[mesh.indices[i]];
            const auto& b=mesh.vertices[mesh.indices[i+1]];
            const auto& c=mesh.vertices[mesh.indices[i+2]];
            const auto uv1=b.uv-a.uv,uv2=c.uv-a.uv;
            require(std::abs(uv1.x*uv2.y-uv1.y*uv2.x)>1e-5f,"UV triangles must not collapse");
            require(glm::dot(glm::cross(b.position-a.position,c.position-a.position),a.normal)>0,
                    "Index winding must match the outward normal");
        }
    }
    for(size_t alignment:{size_t(1),size_t(16),size_t(64),size_t(256),size_t(512)}) {
        const auto stride=lab3::uniform_stride(sizeof(lab3::ModelUniform),alignment);
        require(stride>=sizeof(lab3::ModelUniform)&&stride%alignment==0,"Model stride must satisfy device alignment");
        require(stride-sizeof(lab3::ModelUniform)<alignment,"Padding must be minimal");
        for(size_t i=0;i<3;++i)require(i*stride+sizeof(lab3::ModelUniform)<=3*stride,"Dynamic range must fit allocation");
    }
    bool overflow=false;
    try {lab3::uniform_stride(std::numeric_limits<size_t>::max(),256);}catch(const std::runtime_error&){overflow=true;}
    require(overflow,"Uniform stride arithmetic must reject overflow");
    lab3::TextureSettings settings;settings.animated=true;settings.specular=false;
    settings.emissive=true;settings.tiling={3,4};settings.offset={-1,2};
    auto push=lab3::sampling_push(settings,2.5f);
    require(push.flags.x==1&&push.flags.y==0&&push.flags.z==1,"Map selection belongs to push constants");
    require(close(push.animation.x,2.5f)&&close(push.uv_transform.z,-1),"Push parameters must preserve time and UV");
    static_assert(offsetof(lab3::SamplingPush,animation)==16);
    static_assert(offsetof(lab3::SamplingPush,uv_transform)==32);
    static_assert(offsetof(lab3::SamplingPush,tint)==48);
    const auto directory=std::filesystem::path(LAB_TEXTURE_DIR);
    const auto black=lab3::load_image(directory/"black.png");
    require(black.width==1&&black.height==1,"Fallback must be a single pixel");
    require(black.rgba==std::vector<uint8_t>({0,0,0,255}),"Fallback must be opaque black");
    require(lab3::material_files().size()>=5,"Provide several selectable materials");
    for(const auto& material:lab3::material_files()) {
        for(const auto name:{material.albedo,material.specular,material.emissive}) {
            if(!name)continue;
            const auto image=lab3::load_image(directory/name);
            require(image.width==256&&image.height==256,"Material maps must decode to expected size");
            require(image.rgba.size()==size_t(image.width)*image.height*4,"Decode RGBA regardless of file channels");
            bool nonblack=false,dark=false;
            for(size_t i=0;i<image.rgba.size();i+=4) {
                nonblack|=image.rgba[i]>0||image.rgba[i+1]>0||image.rgba[i+2]>0;
                dark|=image.rgba[i]==0&&image.rgba[i+1]==0&&image.rgba[i+2]==0;
            }
            require(nonblack,"Material image must contain visible texels");
            if(name==material.emissive)require(dark,"Emission must leave some texels unlit");
        }
    }
    bool missing=false;
    try {lab3::load_image(directory/"does-not-exist.png");}catch(const std::runtime_error&){missing=true;}
    require(missing,"Missing texture must produce an actionable error");
    const auto invalid=std::filesystem::temp_directory_path()/"lab3-invalid-image-test.png";
    {std::ofstream file(invalid);file<<"invalid image data";}
    bool corrupt=false;
    try {lab3::load_image(invalid);}catch(const std::runtime_error&){corrupt=true;}
    std::filesystem::remove(invalid);
    require(corrupt,"Corrupt image must be rejected");
    lab3::Scene scene;scene.reset();
    require(scene.objects[1].texture.animated,"Demo must show nontrivial sampling by default");
    require(scene.objects[0].texture.material!=scene.objects[1].texture.material,"Demo must show distinct textures");
}
}
int main(){try{geometry();lighting();camera();contracts();textures_and_buffers();std::cout<<"Geometry, camera, image decoding, UVs, dynamic UBO and push contracts passed\n";}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
