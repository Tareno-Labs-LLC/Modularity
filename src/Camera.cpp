#include "Camera.h"
#ifndef __ANDROID__
#include "ThirdParty/glfw/include/GLFW/glfw3.h"
#endif
void Camera::processMouse(double xpos, double ypos) {
    if (ImGuizmo::IsUsing() || ImGuizmo::IsOver()) return;
    if (firstMouse) {lastX = xpos; lastY = ypos; firstMouse = false;}
    float xoffset = (xpos - lastX); float yoffset = (lastY - ypos);
    lastX = xpos; lastY = ypos;
    processMouseDelta(xoffset, yoffset);
}
void Camera::processMouseDelta(double deltaX, double deltaY) {
    if (ImGuizmo::IsUsing() || ImGuizmo::IsOver()) return;
    float sens = std::max(0.001f, mouseSensitivity);
    float xoffset = static_cast<float>(deltaX) * sens;
    float yoffset = static_cast<float>(deltaY) * sens;
    yaw += xoffset;
    pitch += yoffset;
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
    glm::vec3 direction;
    direction.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    direction.y = sin(glm::radians(pitch));
    direction.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    front = glm::normalize(direction);
}
void Camera::processKeyboard(float deltaTime, GLFWwindow* window) {
#ifdef __ANDROID__
    (void)deltaTime; (void)window; return;
#else
    float currentSpeed = moveSpeed;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {currentSpeed = sprintSpeed;}
    glm::vec3 desiredDir(0.0f);
    bool isMoving = false;
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {desiredDir += front; isMoving = true;}
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {desiredDir -= front; isMoving = true;}
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {desiredDir -= glm::normalize(glm::cross(front, up)); isMoving = true;}
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {desiredDir += glm::normalize(glm::cross(front, up)); isMoving = true;}
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {desiredDir -= up; isMoving = true;}
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) {desiredDir += up; isMoving = true;}
#if !MODULARITY_OPENGL_ES
    if (glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS) {glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);}
    if (glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS) {glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);}
#endif
    if (smoothMovement) {
        glm::vec3 targetVelocity(0.0f);
        if (isMoving) {
            float length = glm::length(desiredDir);
            if (length > 0.0001f) {
                desiredDir = desiredDir / length; targetVelocity = desiredDir * currentSpeed;
            }
        }
        float smoothFactor = 1.0f - std::exp(-acceleration * deltaTime);
        velocity = glm::mix(velocity, targetVelocity, smoothFactor);
        position += velocity * deltaTime;
    } else {
        velocity = glm::vec3(0.0f);
        if (isMoving) {
            float length = glm::length(desiredDir);
            if (length > 0.0001f) {
                desiredDir /= length; position += desiredDir * currentSpeed * deltaTime;
            }
        }
    }
#endif // __ANDROID__
}
glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(position, position + front, up);
}
void ViewportController::updateFocusFromImGui(bool windowFocused, bool cursorLocked) {
    if (cursorLocked) {viewportFocused = true; manualUnfocus = false; return;}
    if (!windowFocused && viewportFocused && !manualUnfocus) {viewportFocused = false;}
}
void ViewportController::setFocused(bool focused)  {viewportFocused = focused;}
bool ViewportController::isViewportFocused() const {return viewportFocused;}
void ViewportController::clearManualUnfocus()      {manualUnfocus = false;}
void ViewportController::update(GLFWwindow* window, bool& cursorLocked) {
    #ifdef __ANDROID__
        (void)window; (void)cursorLocked;
    #else
        if (viewportFocused && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {viewportFocused = false; manualUnfocus = true; cursorLocked = false;}
    #endif
}
