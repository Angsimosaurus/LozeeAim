#pragma once

#include <string>
#include <vector>

namespace runtime_deps {

constexpr int BackendDirectML = 0;
constexpr int BackendTensorRT = 1;
constexpr int BackendCPU = 2;

std::string DepsRootDir();
std::string CommonDir();
std::string BackendDir(int backend);
std::string DepPathsConfigPath();
std::string DependencyPath(const std::string& filename, int backend);
std::string InstallDirForDependency(const std::string& filename, int backend);
std::string TensorRtCudaDir();
std::string TensorRtCudnnDir();
void SetTensorRtCudaDir(const std::string& dir);
void SetTensorRtCudnnDir(const std::string& dir);
std::vector<std::string> TensorRtReferenceDirs();
std::vector<std::string> ListRootLegacyDependencies();
void EnsureDependencyDirs();
bool Configure(int backend, bool use_cpu, std::string* error = nullptr);
bool ConfigureFromConfig(std::string* error = nullptr);
bool PreloadTensorRtProviderDependencies(std::string* error = nullptr);

}  // namespace runtime_deps
