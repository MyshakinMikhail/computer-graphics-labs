#include "application.hpp"
#include "scene.hpp"
#include "renderer.hpp"
#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace application {
namespace {
lab3::Scene scene;
double previous_time = -1;
int selected_point = 0, selected_spot = 0, selected_object = 1;
bool ui_visible = true;

void reset_scene() { scene.reset(); selected_point=selected_spot=0;selected_object=1; }
void basic_scene() {
    reset_scene();
    for(auto& o:scene.objects) {
        o.texture.material=3;o.texture.animated=false;
        o.texture.specular=o.texture.emissive=false;
    }
}
void dark_scene() {
    reset_scene();
    scene.directional_enabled=false;scene.ambient_intensity=0;
    scene.points.clear();scene.spots.clear();scene.show_markers=false;
    scene.objects[0].texture.material=2;
    scene.objects[1].texture.material=1;
}
void color(const char* label, glm::vec3& c) { ImGui::ColorEdit3(label,glm::value_ptr(c)); }
void position(const char* label, glm::vec3& p) { ImGui::DragFloat3(label,glm::value_ptr(p),0.03f); }
void texture_panel() {
    const char* names[]{"Octahedron 1", "Octahedron 2", "Floor"};
    ImGui::Combo("Object",&selected_object,names,3);
    auto& t=scene.objects[size_t(selected_object)].texture;
    const auto& files=lab3::material_files();
    if(ImGui::BeginCombo("Texture",files[size_t(t.material)].name)) {
        for(size_t i=0;i<files.size();++i) {
            const bool selected=t.material==int(i);
            if(ImGui::Selectable(files[i].name,selected)) t.material=int(i);
            if(selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SliderFloat2("UV tiling",glm::value_ptr(t.tiling),0.25f,8,"%.2f");
    ImGui::SliderFloat2("UV offset",glm::value_ptr(t.offset),-2,2,"%.2f");
    ImGui::SeparatorText("Animated sampling");
    ImGui::Checkbox("Blend animated texture layers",&t.animated);
    ImGui::BeginDisabled(!t.animated);
    ImGui::SliderFloat("Blend strength",&t.strength,0,1,"%.2f");
    ImGui::SliderFloat("Texture speed",&t.speed,-2,2,"%.2f");
    color("Modulation tint",t.tint);
    ImGui::EndDisabled();
    ImGui::Checkbox("Run texture animation",&scene.animate_textures);
    if(ImGui::Button("Restart texture animation"))scene.texture_time=0;
    ImGui::SeparatorText("Material maps");
    ImGui::Checkbox("Specular map",&t.specular);
    ImGui::Checkbox("Emissive map",&t.emissive);
    ImGui::SliderFloat("Emission strength",&t.emission,0,5,"%.2f");
    ImGui::Text("Specular: %s",files[size_t(t.material)].specular?"available":"absent (no highlights)");
    ImGui::Text("Emissive: %s",files[size_t(t.material)].emissive?"available":"absent (no glow)");
    ImGui::TextWrapped("Jade energy and Obsidian lava have glowing patterns. Use Glow in dark to see emission without any light.");
}
void lighting_panel() {
    color("Ambient color",scene.ambient_color);
    ImGui::SliderFloat("Ambient intensity",&scene.ambient_intensity,0,0.5f,"%.3f");
    ImGui::SeparatorText("Directional light");
    ImGui::Checkbox("Enabled##directional",&scene.directional_enabled);
    position("Direction",scene.directional_direction);
    color("Light color",scene.directional_color);
    ImGui::SliderFloat("Intensity##directional",&scene.directional_intensity,0,3,"%.2f");
    ImGui::TextWrapped("Direction points from the source towards the scene. Zero vectors use a downward fallback.");
    ImGui::SeparatorText("Presentation");
    ImGui::Checkbox("Show source cubes",&scene.show_markers);
    ImGui::Checkbox("Show spotlight cones",&scene.show_cones);
    ImGui::SliderFloat("Cone guide length",&scene.cone_length,0.2f,8,"%.1f");
    ImGui::Checkbox("Show floor",&scene.show_floor);
    ImGui::Checkbox("Run texture animation",&scene.animate_textures);
    ImGui::Checkbox("Rotate objects",&scene.animate_objects);
    ImGui::Checkbox("Orbit point lights",&scene.animate_lights);
    ImGui::SliderFloat("Animation speed",&scene.animation_speed,0.05f,2,"%.2f");
}
void camera_panel() {
    position("Position",scene.camera.position);
    ImGui::SliderFloat("Yaw",&scene.camera.yaw,-180,180,"%.1f deg");
    ImGui::SliderFloat("Pitch",&scene.camera.pitch,-89,89,"%.1f deg");
    ImGui::SliderFloat("Field of view",&scene.camera.fov,20,100,"%.1f deg");
    ImGui::SliderFloat("Move speed",&scene.camera.speed,0.5f,15,"%.1f");
    if(ImGui::Button("Reset camera")) scene.camera=lab3::Camera{};
    ImGui::Separator();
    ImGui::TextWrapped("Hold right mouse button over the scene to look around. WASD moves, Q/E moves down/up, Shift accelerates. Mouse wheel changes FOV. R resets the camera, Tab hides the panel.");
}
void material_panel() {
    for(size_t i=0;i<scene.objects.size();++i) {
        ImGui::PushID(int(i));
        if(ImGui::CollapsingHeader(i==0?"Octahedron 1":i==1?"Octahedron 2":"Floor",ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& o=scene.objects[i];
            color("Albedo tint",o.material.albedo);
            color("Specular color",o.material.specular);
            ImGui::SliderFloat("Shininess",&o.material.shininess,1,256,"%.0f",ImGuiSliderFlags_Logarithmic);
            position("Position",o.position);
            ImGui::DragFloat3("Rotation",glm::value_ptr(o.rotation),0.5f,-360,360,"%.1f",ImGuiSliderFlags_AlwaysClamp);
            ImGui::DragFloat3("Scale",glm::value_ptr(o.scale),0.01f,0.05f,5,"%.2f",ImGuiSliderFlags_AlwaysClamp);
        }
        ImGui::PopID();
    }
}
void point_panel() {
    ImGui::Text("%zu point lights | GPU capacity: %zu",scene.points.size(),lab3::renderer::point_capacity());
    ImGui::BeginDisabled(scene.points.size()>=lab3::renderer::light_limit(false));
    if(ImGui::Button("Add point light")) {
        lab3::PointLight p; p.position=scene.camera.position+scene.camera.forward()*3.0f;
        scene.points.push_back(p); selected_point=int(scene.points.size()-1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(scene.points.empty());
    if(ImGui::Button("Remove##point")) {
        scene.points.erase(scene.points.begin()+selected_point);
        selected_point=std::max(0,std::min(selected_point,int(scene.points.size())-1));
    }
    ImGui::EndDisabled();
    if(scene.points.empty()) {ImGui::TextWrapped("No point lights. Add one to begin.");return;}
    if(ImGui::BeginChild("Points",ImVec2(0,100),ImGuiChildFlags_Borders)) {
        for(size_t i=0;i<scene.points.size();++i) {
            const std::string label="Point "+std::to_string(i+1);
            if(ImGui::Selectable(label.c_str(),selected_point==int(i)))selected_point=int(i);
        }
    }
    ImGui::EndChild();
    auto& p=scene.points[size_t(selected_point)];
    ImGui::Checkbox("Enabled##point",&p.enabled);
    position("Position",p.position); color("Color",p.color);
    ImGui::DragFloat("Intensity",&p.intensity,0.1f,0,1000,"%.2f",ImGuiSliderFlags_AlwaysClamp);
    ImGui::TextWrapped("Falloff: intensity / distance squared. Orbit animation rotates each source around the world Y axis.");
}
void spot_panel() {
    ImGui::Text("%zu spotlights | GPU capacity: %zu",scene.spots.size(),lab3::renderer::spot_capacity());
    ImGui::BeginDisabled(scene.spots.size()>=lab3::renderer::light_limit(true));
    if(ImGui::Button("Add spotlight")) {
        lab3::SpotLight s; s.position=scene.camera.position;s.direction=scene.camera.forward();
        scene.spots.push_back(s);selected_spot=int(scene.spots.size()-1);
    }
    ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(scene.spots.empty());
    if(ImGui::Button("Remove##spot")) {
        scene.spots.erase(scene.spots.begin()+selected_spot);
        selected_spot=std::max(0,std::min(selected_spot,int(scene.spots.size())-1));
    }
    ImGui::EndDisabled();
    if(scene.spots.empty()){ImGui::TextWrapped("No spotlights. Add one to begin.");return;}
    if(ImGui::BeginChild("Spots",ImVec2(0,90),ImGuiChildFlags_Borders)) {
        for(size_t i=0;i<scene.spots.size();++i){
            const std::string label="Spotlight "+std::to_string(i+1);
            if(ImGui::Selectable(label.c_str(),selected_spot==int(i)))selected_spot=int(i);
        }
    }
    ImGui::EndChild();
    auto& s=scene.spots[size_t(selected_spot)];
    ImGui::Checkbox("Enabled##spot",&s.enabled);
    position("Position",s.position);position("Direction",s.direction);color("Color",s.color);
    ImGui::DragFloat("Intensity",&s.intensity,0.1f,0,1000,"%.2f",ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Inner half-angle",&s.inner_angle,0.5f,s.outer_angle,"%.1f deg");
    ImGui::SliderFloat("Outer half-angle",&s.outer_angle,s.inner_angle,85,"%.1f deg");
    if(ImGui::Button("Aim at origin"))s.direction=lab3::safe_direction(-s.position);
    ImGui::SameLine();
    if(ImGui::Button("Copy camera pose")){s.position=scene.camera.position;s.direction=scene.camera.forward();}
    ImGui::Checkbox("Show source cubes",&scene.show_markers);
    ImGui::Checkbox("Show cone guides",&scene.show_cones);
    ImGui::TextWrapped("White rings show inner and outer cone angles. Equal angles give a hard edge. Guides are unlit and do not affect illumination.");
}
void interface() {
    ImGui::SetNextWindowPos(ImVec2(18,18),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(410,660),ImGuiCond_FirstUseEver);
    ImGui::Begin("Lab 3 | Texture Mapping",&ui_visible);
    ImGui::TextColored(ImVec4(0.4f,0.85f,0.95f,1),"VARIANT 2 / REGULAR OCTAHEDRON");
    ImGui::TextDisabled("Albedo / Specular / Emission / Animated UV");
    ImGui::Text("%.1f FPS | %zu point / %zu spot",ImGui::GetIO().Framerate,scene.points.size(),scene.spots.size());
    if(ImGui::Button("Reset scene")) reset_scene();
    ImGui::SameLine();
    if(ImGui::Button("Basic texture"))basic_scene();
    ImGui::SameLine();
    if(ImGui::Button("Glow in dark"))dark_scene();
    if(ImGui::BeginTabBar("Controls")) {
        if(ImGui::BeginTabItem("Textures")){texture_panel();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Scene")){lighting_panel();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Camera")){camera_panel();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Material")){material_panel();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Points")){point_panel();ImGui::EndTabItem();}
        if(ImGui::BeginTabItem("Spots")){spot_panel();ImGui::EndTabItem();}
        ImGui::EndTabBar();
    }
    ImGui::End();
}
void input(float dt) {
    const auto& io=ImGui::GetIO();
    if(!io.WantCaptureKeyboard) {
        if(ImGui::IsKeyPressed(ImGuiKey_Tab))ui_visible=!ui_visible;
        if(ImGui::IsKeyPressed(ImGuiKey_R))scene.camera=lab3::Camera{};
        glm::vec3 local{0};
        local.x=float(ImGui::IsKeyDown(ImGuiKey_D))-float(ImGui::IsKeyDown(ImGuiKey_A));
        local.y=float(ImGui::IsKeyDown(ImGuiKey_E))-float(ImGui::IsKeyDown(ImGuiKey_Q));
        local.z=float(ImGui::IsKeyDown(ImGuiKey_W))-float(ImGui::IsKeyDown(ImGuiKey_S));
        scene.camera.move(local,dt,io.KeyShift);
    }
    if(!io.WantCaptureMouse) {
        if(ImGui::IsMouseDown(ImGuiMouseButton_Right))scene.camera.rotate(io.MouseDelta.x*0.18f,io.MouseDelta.y*0.18f);
        scene.camera.fov=std::clamp(scene.camera.fov-io.MouseWheel*2,20.0f,100.0f);
    }
}
} // namespace
bool initialize() {
    reset_scene();
    ImGui::StyleColorsDark();
    auto& style=ImGui::GetStyle();
    style.WindowRounding=8;style.FrameRounding=4;style.GrabRounding=4;
    style.Colors[ImGuiCol_WindowBg]=ImVec4(0.045f,0.06f,0.085f,0.97f);
    style.Colors[ImGuiCol_CheckMark]=ImVec4(0.35f,0.8f,0.9f,1);
    style.Colors[ImGuiCol_SliderGrab]=ImVec4(0.25f,0.65f,0.8f,1);
    try {
        if(lab3::renderer::initialize(scene))return true;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
    lab3::renderer::shutdown();
    return false;
}
void shutdown(){lab3::renderer::shutdown();}
void update(double time) {
    const float dt=previous_time<0?0:float(std::clamp(time-previous_time,0.0,0.1));
    previous_time=time;
    input(dt);
    if(scene.animate_objects)scene.angle=std::remainder(scene.angle+dt*scene.animation_speed,6.283185307f);
    if(scene.animate_lights)scene.light_angle=std::remainder(scene.light_angle+dt*scene.animation_speed,6.283185307f);
    if(scene.animate_textures)scene.texture_time+=dt;
    if(ui_visible)interface();
}
void render(const graphics::internal::FrameData& frame){lab3::renderer::render(frame,scene);}
void exercise(unsigned frame) {
    switch(frame) {
    case 5:
        basic_scene();scene.points.clear();scene.spots.clear();
        std::cout<<"EXERCISE: one shared material set, no effects, empty SSBOs\n";break;
    case 15:
        reset_scene();scene.show_cones=true;
        for(size_t i=0;i<scene.objects.size();++i) {
            scene.objects[i].texture.material=int(i);
            scene.objects[i].texture.animated=true;
        }
        for(int i=0;i<37;++i) {
            const float a=float(i)*6.283185307f/37;
            scene.points.push_back({{3*std::cos(a),2,3*std::sin(a)},{1,0.5f,0.2f},0.02f,true});
            scene.spots.push_back({{3*std::cos(a),3,3*std::sin(a)},{0,-1,0},{0.2f,0.5f,1},0.02f,15,25,true});
        }
        std::cout<<"EXERCISE: distinct materials, animated layers, grow lights and device-local cone buffer\n";break;
    case 30:
        scene.points.resize(1);scene.spots.resize(1);
        scene.spots[0].inner_angle=scene.spots[0].outer_angle=25;
        for(auto& o:scene.objects)o.texture.material=1;
        std::cout<<"EXERCISE: shared descriptor, distinct dynamic model offsets, resize\n";break;
    case 45:
        scene.spots[0].direction={0,0,0};scene.directional_direction={0,0,0};
        scene.objects[0].scale={0.05f,2,0.8f};
        scene.objects[0].texture.tiling={8,0.25f};scene.objects[0].texture.speed=-2;
        scene.objects[1].texture.offset={-2,2};scene.objects[1].texture.strength=1;
        std::cout<<"EXERCISE: nonuniform scale, reverse animation, UV extremes\n";break;
    case 60:
        dark_scene();
        std::cout<<"EXERCISE: emissive maps without any lighting\n";break;
    case 75:
        reset_scene();scene.animate_objects=scene.animate_lights=true;
        scene.objects[0].texture.material=4;scene.objects[2].texture.material=0;
        std::cout<<"EXERCISE: switch remaining material, animate camera scene\n";break;
    case 85:
        scene.animate_textures=false;
        for(auto& o:scene.objects) {
            o.texture.material=3;o.texture.specular=o.texture.emissive=true;
        }
        std::cout<<"EXERCISE: black map fallbacks and paused texture clock\n";break;
    case 95:
        reset_scene();
        std::cout<<"EXERCISE: complete\n";break;
    default:break;
    }
}
} // namespace application
