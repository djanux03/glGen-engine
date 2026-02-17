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

private:
    static void errorCallback(int error, const char* description);
    static void framebufferSizeCallback(GLFWwindow* window, int width, int height);

    std::unique_ptr<AppState> m;
};
