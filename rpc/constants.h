#ifndef SLOP_RPC_CONSTANTS_H_
#define SLOP_RPC_CONSTANTS_H_

namespace slop::rpc::v1 {

inline constexpr const char* kDefaultListenAddr = "127.0.0.1:50051";
inline constexpr const char* kDefaultDbPath = "slop_rpc.db";
inline constexpr const char* kProviderGemini = "gemini";
inline constexpr const char* kProviderOpenAi = "openai";

}  // namespace slop::rpc::v1

#endif  // SLOP_RPC_CONSTANTS_H_
