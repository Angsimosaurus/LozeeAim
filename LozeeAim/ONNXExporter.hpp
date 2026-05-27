#pragma once
// Self-contained ONNX protobuf writer for 3-layer MLP models.
// No external dependencies beyond the C++ standard library.
#include <vector>
#include <cstring>
#include <fstream>
#include <cstdint>
#include <cstdio>

namespace onnx_export {

// ---------------------------------------------------------------------------
// Protobuf wire-format primitives
// ---------------------------------------------------------------------------
inline void pb_varint(std::vector<uint8_t>& b, uint64_t v) {
    while (v > 0x7F) { b.push_back((uint8_t)((v & 0x7F) | 0x80)); v >>= 7; }
    b.push_back((uint8_t)(v & 0x7F));
}
inline void pb_fixed32(std::vector<uint8_t>& b, float v) {
    uint32_t x; std::memcpy(&x, &v, 4);
    b.push_back((uint8_t)(x)); b.push_back((uint8_t)(x >> 8));
    b.push_back((uint8_t)(x >> 16)); b.push_back((uint8_t)(x >> 24));
}
inline void pb_tag(std::vector<uint8_t>& b, int field, int wtype) { pb_varint(b, (field << 3) | wtype); }
inline void pb_int32(std::vector<uint8_t>& b, int field, int32_t v)  { pb_tag(b, field, 0); pb_varint(b, (uint64_t)(uint32_t)v); }
inline void pb_int64(std::vector<uint8_t>& b, int field, int64_t v)  { pb_tag(b, field, 0); pb_varint(b, (uint64_t)v); }
inline void pb_string(std::vector<uint8_t>& b, int field, const std::string& s) {
    pb_tag(b, field, 2); pb_varint(b, s.size()); b.insert(b.end(), s.begin(), s.end());
}
inline void pb_raw(std::vector<uint8_t>& b, int field, const void* d, size_t n) {
    pb_tag(b, field, 2); pb_varint(b, n); b.insert(b.end(), (const uint8_t*)d, (const uint8_t*)d + n);
}
inline void pb_msg(std::vector<uint8_t>& b, int field, const std::vector<uint8_t>& m) {
    pb_tag(b, field, 2); pb_varint(b, m.size()); b.insert(b.end(), m.begin(), m.end());
}

// ---------------------------------------------------------------------------
// TensorProto builder
// ---------------------------------------------------------------------------
inline std::vector<uint8_t> pb_tensor(const std::string& name,
                                       const std::vector<int64_t>& dims,
                                       const float* data, int count) {
    std::vector<uint8_t> b;
    if (!name.empty())      pb_string(b, 8, name);              // name
    for (auto d : dims)     pb_int64(b, 1, d);                  // dims
    pb_int32(b, 2, 1);                                         // data_type = FLOAT
    pb_raw(b, 10, data, count * sizeof(float));                  // raw_data (field 10, NOT 9!)
    return b;
}

// ---------------------------------------------------------------------------
// ValueInfoProto (name + TypeProto)
// ---------------------------------------------------------------------------
inline std::vector<uint8_t> pb_dims(const std::vector<int64_t>& dims) {
    std::vector<uint8_t> b;
    for (auto d : dims) {
        std::vector<uint8_t> dimval; pb_int64(dimval, 1, d);
        pb_msg(b, 1, dimval);
    }
    return b;
}

inline std::vector<uint8_t> pb_value_info(const std::string& name, const std::vector<int64_t>& shape) {
    std::vector<uint8_t> tensor_type;          // TypeProto.Tensor
    pb_int32(tensor_type, 1, 1);              // elem_type = FLOAT
    pb_msg(tensor_type, 2, pb_dims(shape));   // shape
    
    std::vector<uint8_t> type;                 // TypeProto
    pb_msg(type, 11, tensor_type);            // tensor_type

    std::vector<uint8_t> b;                    // ValueInfoProto
    pb_string(b, 1, name);                    // name
    pb_msg(b, 3, type);                       // type
    return b;
}

// ---------------------------------------------------------------------------
// NodeProto
// ---------------------------------------------------------------------------
inline std::vector<uint8_t> pb_node(const std::string& op_type,
                                     const std::vector<std::string>& inputs,
                                     const std::vector<std::string>& outputs) {
    std::vector<uint8_t> b;
    pb_string(b, 2, op_type);
    for (auto& i : inputs)  pb_string(b, 1, i);
    for (auto& o : outputs) pb_string(b, 3, o);
    return b;
}

// ---------------------------------------------------------------------------
// MLP weights descriptor
// ---------------------------------------------------------------------------
struct MLPWeights {
    const float* W1;     // [hidden1 * input]  row-major, W_cpp[i*input+j] = weight from input[j] to hidden1[i]
    const float* b1;     // [hidden1]
    const float* W2;     // [hidden2 * hidden1]
    const float* b2;     // [hidden2]
    const float* W3;     // [output * hidden2]
    const float* b3;     // [output]
    int input_size;
    int hidden1_size;
    int hidden2_size;
    int output_size;
};

// Transpose helper: C++ weight [R][C] → ONNX MatMul weight [C][R]
inline void transpose_weights(const float* src, int R, int C, float* dst) {
    for (int i = 0; i < R; i++)
        for (int j = 0; j < C; j++)
            dst[j * R + i] = src[i * C + j];
}

// ---------------------------------------------------------------------------
// Save MLP to ONNX file
// ---------------------------------------------------------------------------
inline bool SaveMLPToONNX(const char* path, const MLPWeights& w) {
    int I = w.input_size, H1 = w.hidden1_size, H2 = w.hidden2_size, O = w.output_size;

    // Transpose weights for ONNX MatMul convention
    std::vector<float> w1t(I * H1), w2t(H1 * H2), w3t(H2 * O);
    transpose_weights(w.W1, H1, I, w1t.data());
    transpose_weights(w.W2, H2, H1, w2t.data());
    transpose_weights(w.W3, O,  H2, w3t.data());

    // ---- GraphProto ----
    std::vector<uint8_t> graph;

    // name
    pb_string(graph, 2, "mouse_trajectory_nn");

    // input: ValueInfoProto "input" shape [1, I]
    pb_msg(graph, 11, pb_value_info("input", {1, (int64_t)I}));

    // output: ValueInfoProto "output" shape [1, O]
    pb_msg(graph, 12, pb_value_info("output", {1, (int64_t)O}));

    // initializers
    pb_msg(graph, 5, pb_tensor("fc1.weight", {(int64_t)I,   (int64_t)H1},   w1t.data(), I*H1));
    pb_msg(graph, 5, pb_tensor("fc1.bias",   {(int64_t)H1},                  w.b1,      H1));
    pb_msg(graph, 5, pb_tensor("fc2.weight", {(int64_t)H1,  (int64_t)H2},    w2t.data(), H1*H2));
    pb_msg(graph, 5, pb_tensor("fc2.bias",   {(int64_t)H2},                  w.b2,      H2));
    pb_msg(graph, 5, pb_tensor("fc3.weight", {(int64_t)H2,  (int64_t)O},     w3t.data(), H2*O));
    pb_msg(graph, 5, pb_tensor("fc3.bias",   {(int64_t)O},                   w.b3,      O));

    // nodes: MatMul → Add → Tanh × 2, then MatMul → Add (linear output)
    pb_msg(graph, 1, pb_node("MatMul", {"input",    "fc1.weight"}, {"h1_mm"}));
    pb_msg(graph, 1, pb_node("Add",    {"h1_mm",    "fc1.bias"},   {"h1_out"}));
    pb_msg(graph, 1, pb_node("Tanh",   {"h1_out"},                 {"h1_act"}));
    pb_msg(graph, 1, pb_node("MatMul", {"h1_act",   "fc2.weight"}, {"h2_mm"}));
    pb_msg(graph, 1, pb_node("Add",    {"h2_mm",    "fc2.bias"},   {"h2_out"}));
    pb_msg(graph, 1, pb_node("Tanh",   {"h2_out"},                 {"h2_act"}));
    pb_msg(graph, 1, pb_node("MatMul", {"h2_act",   "fc3.weight"}, {"h3_mm"}));
    pb_msg(graph, 1, pb_node("Add",    {"h3_mm",    "fc3.bias"},   {"output"}));

    // ---- ModelProto ----
    std::vector<uint8_t> model;
    pb_int64(model, 1, 8);                           // ir_version
    {
        std::vector<uint8_t> opset;
        pb_string(opset, 1, "");                     // domain
        pb_int64(opset, 2, 13);                      // version
        pb_msg(model, 5, opset);                     // opset_import
    }
    pb_string(model, 7, "LozeeAim NN Trainer");  // producer_name
    pb_msg(model, 8, graph);                         // graph

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)model.data(), model.size());
    return f.good();
}

// ---------------------------------------------------------------------------
// Convenience: save from flat weights (matches MouseTrajectoryNN layout)
// ---------------------------------------------------------------------------
inline bool SaveFromFlatWeights(const char* path,
                                 const float* W1, const float* b1, int H1, int I,
                                 const float* W2, const float* b2, int H2,
                                 const float* W3, const float* b3, int O) {
    MLPWeights w = {W1, b1, W2, b2, W3, b3, I, H1, H2, O};
    return SaveMLPToONNX(path, w);
}

}  // namespace onnx_export
