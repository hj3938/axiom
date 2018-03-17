#pragma once

#include "../Function.h"

namespace MaximCodegen {

    class MixdownFunction : public Function {
    public:
        explicit MixdownFunction(MaximContext *ctx, llvm::Module *module);

        static std::unique_ptr<MixdownFunction> create(MaximContext *context, llvm::Module *module);

    protected:
        std::unique_ptr<Value>
        generate(ComposableModuleClassMethod *method, const std::vector<std::unique_ptr<Value>> &params,
                 std::unique_ptr<VarArg> vararg) override;
    };

}
