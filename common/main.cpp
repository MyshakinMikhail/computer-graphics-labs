#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include "graphics_internal.hpp"
#include "application.hpp"

namespace {
struct Options { unsigned frames = 0; bool exercise = false; };
Options parse_options(int argc, char** argv) {
    Options options;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--frames"&&i+1<argc) {
            const std::string value=argv[++i];size_t end=0;
            const auto count=std::stoul(value,&end);
            if(end!=value.size()||count==0||count>1000000)throw std::runtime_error("Invalid frame count");
            options.frames=unsigned(count);
        } else if(arg=="--exercise") {
#if LAB_NUMBER >= 2
            options.exercise=true;
#else
            throw std::runtime_error("--exercise is available in lab2 and lab3");
#endif
        } else throw std::runtime_error("Usage: lab"+std::to_string(LAB_NUMBER)+" [--frames N] [--exercise]");
    }
    if(options.exercise&&options.frames==0)options.frames=110;
    return options;
}
}
int main(int argc,char** argv) {
    Options options;
    try {options=parse_options(argc,argv);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
    glfwSetErrorCallback([](int code,const char* text){std::cerr<<"GLFW "<<code<<": "<<text<<'\n';});
    if(!glfwInit())return EXIT_FAILURE;
    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
    const std::string title="Computer Graphics | Lab "+std::to_string(LAB_NUMBER);
    GLFWwindow* window=glfwCreateWindow(1440,900,title.c_str(),nullptr,nullptr);
    if(!window){glfwTerminate();return EXIT_FAILURE;}
    glfwSetFramebufferSizeCallback(window,[](GLFWwindow*,int width,int height){
        if(width>0&&height>0)graphics::internal::resize(uint32_t(width),uint32_t(height));
    });
    if(!ImGui::CreateContext()){glfwDestroyWindow(window);glfwTerminate();return EXIT_FAILURE;}
    // Settings are per executable, so the first lab cannot overwrite the second lab's panel layout.
    const std::string settings="lab"+std::to_string(LAB_NUMBER)+"-imgui.ini";
    ImGui::GetIO().IniFilename=options.frames?nullptr:settings.c_str();
    bool backend_ready=false,graphics_ready=false,app_ready=false;
    int result=EXIT_FAILURE;
    try {
        if(!ImGui_ImplGlfw_InitForVulkan(window,true))throw std::runtime_error("Cannot initialize ImGui/GLFW");
        backend_ready=true;
        if(!graphics::internal::initialize(window))throw std::runtime_error("Cannot initialize Vulkan");
        graphics_ready=true;
        if(!application::initialize())throw std::runtime_error("Cannot initialize laboratory");
        app_ready=true;
        unsigned frame=0;
        while(!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            int width=0,height=0;glfwGetFramebufferSize(window,&width,&height);
            if(width==0||height==0){glfwWaitEvents();continue;}
            ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();
#if LAB_NUMBER >= 2
            if(options.exercise) {
                application::exercise(frame);
                if(frame==30)glfwSetWindowSize(window,1024,768);
                if(frame==60)glfwSetWindowSize(window,1440,900);
            }
#endif
            application::update(glfwGetTime());ImGui::Render();
            const auto data=graphics::internal::prepare();
            if(!data.command_buffer||!data.framebuffer)throw std::runtime_error("Cannot acquire Vulkan frame");
            application::render(data);graphics::internal::submitAndPresent();
            ++frame;
            if(options.frames&&frame>=options.frames)break;
        }
        std::cout<<"Rendered "<<frame<<" frames successfully\n";
        result=EXIT_SUCCESS;
    }catch(const std::exception& e){std::cerr<<"Application error: "<<e.what()<<'\n';}
    if(app_ready)application::shutdown();
    if(graphics_ready)graphics::internal::shutdown();
    if(graphics::internal::validationErrorCount()!=0) {
        std::cerr<<graphics::internal::validationErrorCount()<<" Vulkan validation errors\n";
        result=EXIT_FAILURE;
    }
    if(backend_ready)ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();glfwDestroyWindow(window);glfwTerminate();
    return result;
}
