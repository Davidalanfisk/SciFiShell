#include "SceneGraph.h"

namespace sf {

Node* Node::add(std::unique_ptr<Node> child) {
    child->parent = this;
    children_.push_back(std::move(child));
    return children_.back().get();
}

Mat4 Node::localMatrix() const { return Mat4::translate(position) * Mat4::rotateY(yaw) * Mat4::scale(scale); }

Mat4 Node::worldMatrix() const { return parent ? parent->worldMatrix() * localMatrix() : localMatrix(); }

void Node::updateTree(double dt, double time) {
    update(dt, time);
    for (auto& c : children_) c->updateTree(dt, time);
}

void Node::renderTree(Renderer& renderer) {
    renderPass(renderer, false);
    renderPass(renderer, true);  // blended nodes last so opaque geometry cannot overdraw them
}

void Node::renderPass(Renderer& renderer, bool transparentPass) {
    if (!visible) return;
    if (isTransparent() == transparentPass) render(renderer);
    for (auto& c : children_) c->renderPass(renderer, transparentPass);
}

bool Node::rayHit(Vec3 origin, Vec3 dir, float& t) const {
    float u, v;
    return rayHitUV(origin, dir, t, u, v);
}

bool Node::rayHitUV(Vec3 origin, Vec3 dir, float& t, float& u, float& v) const {
    Mat4 w = worldMatrix();
    Vec3 center = w.transformPoint({0, 0, 0});
    Vec3 ax = w.transformDir({1, 0, 0}), ay = w.transformDir({0, 1, 0});
    Vec3 n = normalize(cross(ax, ay));
    float denom = dot(n, dir);
    if (std::fabs(denom) < 1e-6f) return false;
    float d = dot(center - origin, n) / denom;
    if (d < 0) return false;
    Vec3 rel = origin + dir * d - center;
    u = dot(rel, ax) / dot(ax, ax);
    v = dot(rel, ay) / dot(ay, ay);
    if (std::fabs(u) > 0.5f || std::fabs(v) > 0.5f) return false;
    t = d;
    return true;
}

static void pickRecursive(Node& n, Vec3 o, Vec3 d, Node*& best, float& bestT) {
    if (!n.visible) return;
    float t;
    if (n.pickable && n.rayHit(o, d, t) && t < bestT) {
        best = &n;
        bestT = t;
    }
    for (auto& c : n.children()) pickRecursive(*c, o, d, best, bestT);
}

Node* pickNode(Node& root, Vec3 origin, Vec3 dir, float* distance) {
    Node* best = nullptr;
    float bestT = 1e9f;
    pickRecursive(root, origin, normalize(dir), best, bestT);
    if (distance) *distance = bestT;
    return best;
}

}  // namespace sf
