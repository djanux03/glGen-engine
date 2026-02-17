#pragma once
#include <memory>

struct GLFWwindow;

// PImpl state lives in App.cpp
struct AppState;

class App
{
public:
    App();
    ~App();

    int run();

    static void errorCallback(int error, const char* description);
    static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void dropCallback(GLFWwindow* window, int pathCount, const char** paths);

private:
    std::unique_ptr<AppState> m;
};
