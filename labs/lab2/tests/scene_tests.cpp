#include "scene.hpp"
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
    const auto mesh=lab2::octahedron_mesh();require(mesh.vertices.size()==24,"Octahedron must have eight triangles");
    for(size_t i=0;i<mesh.vertices.size();i+=3) {
        const auto a=mesh.vertices[i],b=mesh.vertices[i+1],c=mesh.vertices[i+2];
        require(close(glm::length(a.normal),1),"Normals must be unit vectors");
        require(glm::dot(a.normal,(a.position+b.position+c.position)/3.0f)>0,"Normals must point outwards");
        require(close(glm::dot(a.normal,b.position-a.position),0),"Normal must be perpendicular to the face");
        require(close(glm::length(a.position-b.position),std::sqrt(2.0f)),"Octahedron edges must be equal");
    }
    for(const auto& v:lab2::floor_mesh().vertices)require(v.normal.y>0.99f,"Floor normals must face up");
    // Nonuniform scale: transformed normal must remain perpendicular to transformed edges.
    lab2::Object object;object.scale={0.2f,2,0.7f};object.rotation={12,40,20};
    const auto m=object.model();const auto n=glm::transpose(glm::inverse(glm::mat3(m)));
    const auto& a=mesh.vertices[0];const auto& b=mesh.vertices[1];
    require(close(glm::dot(n*a.normal,glm::mat3(m)*(b.position-a.position)),0),"Normal matrix must handle nonuniform scale");
}
void lighting() {
    require(close(lab2::inverse_square(4)/lab2::inverse_square(16),4),"Double distance must reduce intensity four times");
    require(std::isfinite(lab2::inverse_square(0)),"Source coincidence must not divide by zero");
    const float inner=std::cos(glm::radians(15.0f)),outer=std::cos(glm::radians(30.0f));
    require(close(lab2::cone_weight(1,inner,outer),1),"Cone center must have full intensity");
    require(close(lab2::cone_weight(outer-0.1f,inner,outer),0),"Outside cone must be dark");
    require(close(lab2::cone_weight((inner+outer)/2,inner,outer),0.5f),"Cone penumbra must interpolate");
    require(close(lab2::cone_weight(outer,outer,outer),1),"Equal angles must support hard edge");
    require(close(lab2::cone_weight(outer-0.001f,outer,outer),0),"Hard edge must reject outside cone");
    require(close(glm::length(lab2::safe_direction({0,0,0})),1),"Zero direction must have valid fallback");
}
void camera() {
    lab2::Camera c;c.rotate(1000,1000);require(c.pitch>=-89&&c.pitch<=89,"Pitch must be clamped");
    require(close(glm::length(c.forward()),1),"Camera forward must be normalized");
    require(finite(c.view_projection(16.0f/9)),"Camera must produce finite matrix at pitch limit");
    c=lab2::Camera{};const auto start=c.position;c.move({0,0,1},0.1f,false);
    require(close(glm::length(c.position-start),c.speed*0.1f),"Camera speed must use delta time");
    c.position=start;c.move({1,0,1},0.1f,false);
    require(close(glm::length(c.position-start),c.speed*0.1f),"Diagonal movement must not be faster");
}
void contracts() {
    static_assert(offsetof(lab2::SceneUniform,camera_position)==64);
    static_assert(offsetof(lab2::SceneUniform,light_counts)==128);
    static_assert(offsetof(lab2::ModelUniform,albedo)==128);
    static_assert(offsetof(lab2::SpotGPU,outer)==48);
    lab2::Scene s;s.reset();s.points.clear();s.spots.clear();
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
}
int main(){try{geometry();lighting();camera();contracts();std::cout<<"Geometry, lighting, camera and GPU contracts passed\n";}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
