#include "Mouse.h"

double Mouse::x = 0;
double Mouse::y = 0;

double Mouse::lastX = 0;
double Mouse::lastY = 0;

double Mouse::dx = 0;
double Mouse::dy = 0;

double Mouse::scrollDX = 0;
double Mouse::scrollDY = 0;

bool Mouse::firstMouse = true;
static bool manualMode = false;

bool Mouse::buttons[GLFW_MOUSE_BUTTON_LAST + 1] = {0};
bool Mouse::buttonsChanged[GLFW_MOUSE_BUTTON_LAST + 1] = {0};

void Mouse::cursorPosCallback(GLFWwindow *window, double _x, double _y) {
  x = _x;
  y = _y;

  if (firstMouse) {
    lastX = x;
    lastY = y;
    firstMouse = false;
  }

  if (!manualMode) {
    dx += x - lastX;
    dy += lastY - y;
  }
  lastX = x;
  lastY = y;
}
void Mouse::mouseButtonCallback(GLFWwindow *window, int button, int action,
                                int mods) {
  if (button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
    return;

  if (action == GLFW_PRESS)
    buttons[button] = true;
  if (action == GLFW_RELEASE)
    buttons[button] = false;

  buttonsChanged[button] = (action != GLFW_REPEAT);
}

void Mouse::mouseWheelCallback(GLFWwindow *window, double _dx, double _dy) {
  scrollDX += _dx;
  scrollDY += _dy;
}

double Mouse::getMouseX() { return x; }
double Mouse::getMouseY() { return y; }
double Mouse::getDX() {
  double _dx = dx;
  dx = 0;
  return _dx;
}
double Mouse::getDY() {
  double _dy = dy;
  dy = 0;
  return _dy;
}
double Mouse::getScrollDX() {
  double dx = scrollDX;
  scrollDX = 0;
  return dx;
}
double Mouse::getSCrollDY() {
  double dy = scrollDY;
  scrollDY = 0;
  return dy;
}
bool Mouse::button(int button) { return buttons[button]; }
bool Mouse::buttonChanged(int button) {
  bool ret = buttonsChanged[button];
  buttonsChanged[button] = false;
  return ret;
}
bool Mouse::buttonWentUp(int button) {
  return !buttons[button] && buttonChanged(button);
}
bool Mouse::buttonWentDown(int button) {
  return buttons[button] && buttonChanged(button);
}

void Mouse::resetDeltas() {
  dx = 0;
  dy = 0;
  scrollDX = 0;
  scrollDY = 0;
}

void Mouse::resetPosition(double _x, double _y) {
  x = _x;
  y = _y;
  lastX = _x;
  lastY = _y;
  firstMouse = false;
  resetDeltas();
}

void Mouse::setDeltas(double _dx, double _dy) {
  dx = _dx;
  dy = _dy;
}

void Mouse::setManualMode(bool enabled) { manualMode = enabled; }
