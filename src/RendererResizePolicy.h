#pragma once

namespace ModuRenderer {

struct ResizeRequest {
    int width = 0;
    int height = 0;
    int currentWidth = 0;
    int currentHeight = 0;
    bool initialized = false;
    bool resizeInProgress = false;
    bool shuttingDown = false;
};

enum class ResizeDecision {
    Accept,
    RejectInvalidSize,
    RejectShuttingDown,
    RejectReentrant,
    RejectNotInitialized,
    RejectUnchanged,
};

inline ResizeDecision EvaluateResizeRequest(const ResizeRequest& request) {
    if (request.width <= 0 || request.height <= 0) {
        return ResizeDecision::RejectInvalidSize;
    }
    if (request.shuttingDown) {
        return ResizeDecision::RejectShuttingDown;
    }
    if (request.resizeInProgress) {
        return ResizeDecision::RejectReentrant;
    }
    if (!request.initialized) {
        return ResizeDecision::RejectNotInitialized;
    }
    if (request.width == request.currentWidth && request.height == request.currentHeight) {
        return ResizeDecision::RejectUnchanged;
    }
    return ResizeDecision::Accept;
}

inline const char* ResizeDecisionName(ResizeDecision decision) {
    switch (decision) {
        case ResizeDecision::Accept:               return "accept";
        case ResizeDecision::RejectInvalidSize:    return "reject:invalid-size";
        case ResizeDecision::RejectShuttingDown:   return "reject:shutting-down";
        case ResizeDecision::RejectReentrant:      return "reject:reentrant";
        case ResizeDecision::RejectNotInitialized: return "reject:not-initialized";
        case ResizeDecision::RejectUnchanged:      return "reject:unchanged";
    }
    return "reject:unknown";
}

inline void ClampResizeToLimit(int& width, int& height, int maxDimension) {
    const int limit = (maxDimension > 0) ? maxDimension : 16384;
    if (width > limit) width = limit;
    if (height > limit) height = limit;
}

}  // namespace ModuRenderer
