#pragma once
// Minimal hierarchical scene graph. Nodes own their children, carry a transform
// (position, yaw, scale), and are updated and rendered recursively.
#include <memory>
#include <string>
#include <vector>

#include "Renderer.h"
#include "Utils.h"

namespace sf {

class Node {
public:
    virtual ~Node() = default;

    std::string name;
    Vec3 position{0, 0, 0};
    Vec3 scale{1, 1, 1};
    float yaw = 0;
    bool visible = true;
    bool pickable = false;  // pickable nodes are treated as rectangles in their local XY plane
    Node* parent = nullptr;

    Node* add(std::unique_ptr<Node> child);
    void clearChildren() { children_.clear(); }
    const std::vector<std::unique_ptr<Node>>& children() const { return children_; }

    Mat4 localMatrix() const;
    Mat4 worldMatrix() const;

    void updateTree(double dt, double time);
    void renderTree(Renderer& renderer);
    void renderPass(Renderer& renderer, bool transparentPass);
    virtual bool isTransparent() const { return false; }

    virtual void update(double dt, double time) { (void)dt; (void)time; }
    virtual void render(Renderer& renderer) { (void)renderer; }
    virtual bool isCard() const { return false; }  // RTTI is disabled by Diligent's build settings

    // Ray vs. this node's unit rectangle (front or back). Returns hit distance via t.
    bool rayHit(Vec3 origin, Vec3 dir, float& t) const;
    // Same, also returning the hit point in -0.5..0.5 rectangle coordinates (u right, v up).
    bool rayHitUV(Vec3 origin, Vec3 dir, float& t, float& u, float& v) const;

private:
    std::vector<std::unique_ptr<Node>> children_;
};

// Returns the closest pickable node hit by the ray (or nullptr).
Node* pickNode(Node& root, Vec3 origin, Vec3 dir, float* distance = nullptr);

}  // namespace sf
