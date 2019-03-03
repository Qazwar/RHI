#pragma once
#include "RHIChooseImpl.h"
#include <EnumClass.h>
#include <RefBase.h>
#include <cstdint>

namespace Nome::RHI
{

enum class EBufferUsageFlags
{
    VertexBuffer,
    IndexBuffer,
    ConstantBuffer,
    Streaming,
};

DEFINE_ENUM_CLASS_BITWISE_OPERATORS(EBufferUsageFlags);

template <typename TDerived>
class CBufferBase : public tc::TLightRefBase<CBufferBase<TDerived>>
{
protected:
    CBufferBase(uint32_t size, EBufferUsageFlags usage, void* initialData = nullptr);

public:
    virtual ~CBufferBase() = default;

    void* Map(size_t offset, size_t size);
    void Unmap();

private:
    uint32_t Size;
    EBufferUsageFlags Usage;
};

using CBuffer = TChooseImpl<CBufferBase>::TConcreteBase;

} /* namespace Nome::RHI */
