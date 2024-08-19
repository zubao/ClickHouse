#include <Columns/ColumnConst.h>
#include <Columns/ColumnString.h>
#include <DataTypes/DataTypeString.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/GatherUtils/Sources.h>
#include <Functions/IFunction.h>
#include <Common/StringUtils.h>
#include <Common/UTF8Helpers.h>

namespace DB
{

namespace
{

/// If 'is_utf8' - measure offset and length in code points instead of bytes.
/// Syntax:
/// - overlay(input, replace, offset[, length])
/// - overlayUTF8(input, replace, offset[, length]) - measure offset and length in code points instead of bytes
template <bool is_utf8>
class FunctionOverlay : public IFunction
{
public:
    static constexpr auto name = is_utf8 ? "overlayUTF8" : "overlay";

    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionOverlay>(); }
    String getName() const override { return name; }
    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }
    bool useDefaultImplementationForConstants() const override { return true; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        FunctionArgumentDescriptors mandatory_args{
            {"input", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isString), nullptr, "String"},
            {"replace", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isString), nullptr, "String"},
            {"offset", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isNativeInteger), nullptr, "(U)Int8/16/32/64"},
        };

        FunctionArgumentDescriptors optional_args{
            {"length", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isNativeInteger), nullptr, "(U)Int8/16/32/64"},
        };

        validateFunctionArguments(*this, arguments, mandatory_args, optional_args);

        return std::make_shared<DataTypeString>();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
    {
        if (input_rows_count == 0)
            return ColumnString::create();

        const size_t number_of_arguments = arguments.size();
        bool has_three_args = number_of_arguments == 3;

        ColumnPtr column_offset = arguments[2].column;
        ColumnPtr column_length;
        if (!has_three_args)
            column_length = arguments[3].column;

        const ColumnConst * column_offset_const = checkAndGetColumn<ColumnConst>(column_offset.get());
        const ColumnConst * column_length_const = nullptr;
        if (!has_three_args)
            column_length_const = checkAndGetColumn<ColumnConst>(column_length.get());

        bool offset_is_const = false;
        bool length_is_const = false;
        Int64 offset = -1;
        Int64 length = -1;
        if (column_offset_const)
        {
            offset = column_offset_const->getInt(0);
            offset_is_const = true;
        }

        if (column_length_const)
        {
            length = column_length_const->getInt(0);
            length_is_const = true;
        }


        auto res_col = ColumnString::create();
        auto & res_data = res_col->getChars();
        auto & res_offsets = res_col->getOffsets();
        res_offsets.resize_exact(input_rows_count);

        ColumnPtr column_input = arguments[0].column;
        ColumnPtr column_replace = arguments[1].column;

        const auto * column_input_const = checkAndGetColumn<ColumnConst>(column_input.get());
        const auto * column_input_string = checkAndGetColumn<ColumnString>(column_input.get());
        if (column_input_const)
        {
            StringRef input = column_input_const->getDataAt(0);
            res_data.reserve((input.size + 1) * input_rows_count);
        }
        else
        {
            res_data.reserve(column_input_string->getChars().size());
        }

        const auto * column_replace_const = checkAndGetColumn<ColumnConst>(column_replace.get());
        const auto * column_replace_string = checkAndGetColumn<ColumnString>(column_replace.get());
        bool input_is_const = (column_input_const != nullptr);
        bool replace_is_const = (column_replace_const != nullptr);

#define OVERLAY_EXECUTE_CASE(THREE_ARGS, OFFSET_IS_CONST, LENGTH_IS_CONST) \
    if (input_is_const && replace_is_const) \
        constantConstant<THREE_ARGS, OFFSET_IS_CONST, LENGTH_IS_CONST>( \
            input_rows_count, \
            column_input_const->getDataAt(0), \
            column_replace_const->getDataAt(0), \
            column_offset, \
            column_length, \
            offset, \
            length, \
            res_data, \
            res_offsets); \
    else if (input_is_const && !replace_is_const) \
        constantVector<THREE_ARGS, OFFSET_IS_CONST, LENGTH_IS_CONST>( \
            input_rows_count, \
            column_input_const->getDataAt(0), \
            column_replace_string->getChars(), \
            column_replace_string->getOffsets(), \
            column_offset, \
            column_length, \
            offset, \
            length, \
            res_data, \
            res_offsets); \
    else if (!input_is_const && replace_is_const) \
        vectorConstant<THREE_ARGS, OFFSET_IS_CONST, LENGTH_IS_CONST>( \
            input_rows_count, \
            column_input_string->getChars(), \
            column_input_string->getOffsets(), \
            column_replace_const->getDataAt(0), \
            column_offset, \
            column_length, \
            offset, \
            length, \
            res_data, \
            res_offsets); \
    else \
        vectorVector<THREE_ARGS, OFFSET_IS_CONST, LENGTH_IS_CONST>( \
            input_rows_count, \
            column_input_string->getChars(), \
            column_input_string->getOffsets(), \
            column_replace_string->getChars(), \
            column_replace_string->getOffsets(), \
            column_offset, \
            column_length, \
            offset, \
            length, \
            res_data, \
            res_offsets);

        if (has_three_args)
        {
            if (offset_is_const)
            {
                OVERLAY_EXECUTE_CASE(true, true, false)
            }
            else
            {
                OVERLAY_EXECUTE_CASE(true, false, false)
            }
        }
        else
        {
            if (offset_is_const && length_is_const)
            {
                OVERLAY_EXECUTE_CASE(false, true, true)
            }
            else if (offset_is_const && !length_is_const)
            {
                OVERLAY_EXECUTE_CASE(false, true, false)
            }
            else if (!offset_is_const && length_is_const)
            {
                OVERLAY_EXECUTE_CASE(false, false, true)
            }
            else
            {
                OVERLAY_EXECUTE_CASE(false, false, false)
            }
        }
#undef OVERLAY_EXECUTE_CASE

        return res_col;
    }


private:
    /// input offset is 1-based, maybe negative
    /// output result is 0-based valid offset, within [0, input_size]
    static size_t getValidOffset(Int64 offset, size_t input_size)
    {
        if (offset > 0)
        {
            if (static_cast<size_t>(offset) > input_size + 1) [[unlikely]]
                return input_size;
            else
                return offset - 1;
        }
        else
        {
            if (input_size < -static_cast<size_t>(offset)) [[unlikely]]
                return 0;
            else
                return input_size + offset;
        }
    }

    /// get character count of a slice [data, data+bytes)
    static size_t getSliceSize(const UInt8 * data, size_t bytes)
    {
        if constexpr (is_utf8)
            return UTF8::countCodePoints(data, bytes);
        else
            return bytes;
    }

    template <bool has_three_args, bool offset_is_const, bool length_is_const>
    void constantConstant(
        size_t rows,
        const StringRef & input,
        const StringRef & replace,
        const ColumnPtr & column_offset,
        const ColumnPtr & column_length,
        Int64 const_offset,
        Int64 const_length,
        ColumnString::Chars & res_data,
        ColumnString::Offsets & res_offsets) const
    {
        if (!has_three_args && length_is_const && const_length < 0)
        {
            constantConstant<true, offset_is_const, false>(
                rows, input, replace, column_offset, column_length, const_offset, -1, res_data, res_offsets);
            return;
        }

        size_t input_size = getSliceSize(reinterpret_cast<const UInt8 *>(input.data), input.size);
        size_t valid_offset = 0; // start from 0, not negative
        if constexpr (offset_is_const)
            valid_offset = getValidOffset(const_offset, input_size);

        size_t replace_size = getSliceSize(reinterpret_cast<const UInt8 *>(replace.data), replace.size);
        size_t valid_length = 0; // not negative
        if constexpr (!has_three_args && length_is_const)
        {
            assert(const_length >= 0);
            valid_length = const_length;
        }
        else if constexpr (has_three_args)
        {
            valid_length = replace_size;
        }

        Int64 offset = 0; // start from 1, maybe negative
        Int64 length = 0; // maybe negative
        const UInt8 * input_begin = reinterpret_cast<const UInt8 *>(input.data);
        const UInt8 * input_end = reinterpret_cast<const UInt8 *>(input.data + input.size);
        size_t res_offset = 0;
        for (size_t i = 0; i < rows; ++i)
        {
            if constexpr (!offset_is_const)
            {
                offset = column_offset->getInt(i);
                valid_offset = getValidOffset(offset, input_size);
            }

            if constexpr (!has_three_args && !length_is_const)
            {
                length = column_length->getInt(i);
                valid_length = length >= 0 ? length : replace_size;
            }

            size_t prefix_size = valid_offset;
            size_t suffix_size = prefix_size + valid_length > input_size ? 0 : input_size - prefix_size - valid_length;

            if constexpr (!is_utf8)
            {
                size_t new_res_size = res_data.size() + prefix_size + replace_size + suffix_size + 1; /// +1 for zero terminator
                res_data.resize(new_res_size);

                /// copy prefix before replaced region
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], input.data, prefix_size);
                res_offset += prefix_size;

                /// copy replace
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], replace.data, replace_size);
                res_offset += replace_size;

                /// copy suffix after replaced region. It is not necessary to copy if suffix_size is zero.
                if (suffix_size)
                {
                    memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], input.data + prefix_size + valid_length, suffix_size);
                    res_offset += suffix_size;
                }
            }
            else
            {
                const auto * prefix_end = GatherUtils::UTF8StringSource::skipCodePointsForward(input_begin, prefix_size, input_end);
                size_t prefix_bytes = prefix_end > input_end ? input.size : prefix_end - input_begin;

                const auto * suffix_begin = GatherUtils::UTF8StringSource::skipCodePointsBackward(input_end, suffix_size, input_begin);
                size_t suffix_bytes = input_end - suffix_begin;

                size_t new_res_size = res_data.size() + prefix_bytes + replace.size + suffix_bytes + 1; /// +1 for zero terminator
                res_data.resize(new_res_size);

                /// copy prefix before replaced region
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], input_begin, prefix_bytes);
                res_offset += prefix_bytes;

                /// copy replace
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], replace.data, replace.size);
                res_offset += replace.size;

                /// copy suffix after replaced region. It is not necessary to copy if suffix_bytes is zero.
                if (suffix_bytes)
                {
                    memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], suffix_begin, suffix_bytes);
                    res_offset += suffix_bytes;
                }
            }

            /// add zero terminator
            res_data[res_offset] = 0;
            ++res_offset;
            res_offsets[i] = res_offset;
        }
    }

    template <bool has_three_args, bool offset_is_const, bool length_is_const>
    void vectorConstant(
        size_t rows,
        const ColumnString::Chars & input_data,
        const ColumnString::Offsets & input_offsets,
        const StringRef & replace,
        const ColumnPtr & column_offset,
        const ColumnPtr & column_length,
        Int64 const_offset,
        Int64 const_length,
        ColumnString::Chars & res_data,
        ColumnString::Offsets & res_offsets) const
    {
        if (!has_three_args && length_is_const && const_length < 0)
        {
            vectorConstant<true, offset_is_const, false>(
                rows, input_data, input_offsets, replace, column_offset, column_length, const_offset, -1, res_data, res_offsets);
            return;
        }

        size_t replace_size = getSliceSize(reinterpret_cast<const UInt8 *>(replace.data), replace.size);
        Int64 length = 0; // maybe negative
        size_t valid_length = 0; // not negative
        if constexpr (!has_three_args && length_is_const)
        {
            assert(const_length >= 0);
            valid_length = const_length;
        }
        else if constexpr (has_three_args)
        {
            valid_length = replace_size;
        }

        Int64 offset = 0; // start from 1, maybe negative
        size_t valid_offset = 0; // start from 0, not negative
        size_t res_offset = 0;
        for (size_t i = 0; i < rows; ++i)
        {
            size_t input_offset = input_offsets[i - 1];
            size_t input_bytes = input_offsets[i] - input_offsets[i - 1] - 1;
            size_t input_size = getSliceSize(&input_data[input_offset], input_bytes);

            if constexpr (offset_is_const)
            {
                valid_offset = getValidOffset(const_offset, input_size);
            }
            else
            {
                offset = column_offset->getInt(i);
                valid_offset = getValidOffset(offset, input_size);
            }

            if constexpr (!has_three_args && !length_is_const)
            {
                length = column_length->getInt(i);
                valid_length = length >= 0 ? length : replace_size;
            }

            size_t prefix_size = valid_offset;
            size_t suffix_size = prefix_size + valid_length > input_size ? 0 : input_size - prefix_size - valid_length;

            if constexpr (!is_utf8)
            {
                size_t new_res_size = res_data.size() + prefix_size + replace_size + suffix_size + 1; /// +1 for zero terminator
                res_data.resize(new_res_size);

                /// copy prefix before replaced region
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], &input_data[input_offset], prefix_size);
                res_offset += prefix_size;

                /// copy replace
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], replace.data, replace_size);
                res_offset += replace_size;

                /// copy suffix after replaced region. It is not necessary to copy if suffix_size is zero.
                if (suffix_size)
                {
                    memcpySmallAllowReadWriteOverflow15(
                        &res_data[res_offset], &input_data[input_offset + prefix_size + valid_length], suffix_size);
                    res_offset += suffix_size;
                }
            }
            else
            {
                const auto * input_begin = &input_data[input_offset];
                const auto * input_end = &input_data[input_offset + input_bytes];
                const auto * prefix_end = GatherUtils::UTF8StringSource::skipCodePointsForward(input_begin, prefix_size, input_end);
                size_t prefix_bytes = prefix_end > input_end ? input_bytes : prefix_end - input_begin;
                const auto * suffix_begin = GatherUtils::UTF8StringSource::skipCodePointsBackward(input_end, suffix_size, input_begin);
                size_t suffix_bytes = input_end - suffix_begin;

                size_t new_res_size = res_data.size() + prefix_bytes + replace.size + suffix_bytes + 1; /// +1 for zero terminator
                res_data.resize(new_res_size);

                /// copy prefix before replaced region
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], &input_data[input_offset], prefix_bytes);
                res_offset += prefix_bytes;

                /// copy replace
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], replace.data, replace.size);
                res_offset += replace.size;

                /// copy suffix after replaced region. It is not necessary to copy if suffix_bytes is zero.
                if (suffix_bytes)
                {
                    memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], suffix_begin, suffix_bytes);
                    res_offset += suffix_bytes;
                }
            }

            /// add zero terminator
            res_data[res_offset] = 0;
            ++res_offset;
            res_offsets[i] = res_offset;
        }
    }

    template <bool has_three_args, bool offset_is_const, bool length_is_const>
    void constantVector(
        size_t rows,
        const StringRef & input,
        const ColumnString::Chars & replace_data,
        const ColumnString::Offsets & replace_offsets,
        const ColumnPtr & column_offset,
        const ColumnPtr & column_length,
        Int64 const_offset,
        Int64 const_length,
        ColumnString::Chars & res_data,
        ColumnString::Offsets & res_offsets) const
    {
        if (!has_three_args && length_is_const && const_length < 0)
        {
            constantVector<true, offset_is_const, false>(
                rows, input, replace_data, replace_offsets, column_offset, column_length, const_offset, -1, res_data, res_offsets);
            return;
        }

        size_t input_size = getSliceSize(reinterpret_cast<const UInt8 *>(input.data), input.size);
        size_t valid_offset = 0; // start from 0, not negative
        if constexpr (offset_is_const)
            valid_offset = getValidOffset(const_offset, input_size);

        Int64 length = 0; // maybe negative
        size_t valid_length = 0; // not negative
        if constexpr (!has_three_args && length_is_const)
        {
            assert(const_length >= 0);
            valid_length = const_length;
        }

        const auto * input_begin = reinterpret_cast<const UInt8 *>(input.data);
        const auto * input_end = reinterpret_cast<const UInt8 *>(input.data + input.size);
        Int64 offset = 0; // start from 1, maybe negative
        size_t res_offset = 0;
        for (size_t i = 0; i < rows; ++i)
        {
            size_t replace_offset = replace_offsets[i - 1];
            size_t replace_bytes = replace_offsets[i] - replace_offsets[i - 1] - 1;
            size_t replace_size = getSliceSize(&replace_data[replace_offset], replace_bytes);

            if constexpr (!offset_is_const)
            {
                offset = column_offset->getInt(i);
                valid_offset = getValidOffset(offset, input_size);
            }

            if constexpr (has_three_args)
            {
                valid_length = replace_size;
            }
            else if constexpr (!length_is_const)
            {
                length = column_length->getInt(i);
                valid_length = length >= 0 ? length : replace_size;
            }

            size_t prefix_size = valid_offset;
            size_t suffix_size = prefix_size + valid_length > input_size ? 0 : input_size - prefix_size - valid_length;

            if constexpr (!is_utf8)
            {
                size_t new_res_size = res_data.size() + prefix_size + replace_size + suffix_size + 1; /// +1 for zero terminator
                res_data.resize(new_res_size);

                /// copy prefix before replaced region
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], input.data, prefix_size);
                res_offset += prefix_size;

                /// copy replace
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], &replace_data[replace_offset], replace_size);
                res_offset += replace_size;

                /// copy suffix after replaced region. It is not necessary to copy if suffix_size is zero.
                if (suffix_size)
                {
                    memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], input.data + prefix_size + valid_length, suffix_size);
                    res_offset += suffix_size;
                }
            }
            else
            {
                const auto * prefix_end = GatherUtils::UTF8StringSource::skipCodePointsForward(input_begin, prefix_size, input_end);
                size_t prefix_bytes = prefix_end > input_end ? input.size : prefix_end - input_begin;
                const auto * suffix_begin = GatherUtils::UTF8StringSource::skipCodePointsBackward(input_end, suffix_size, input_begin);
                size_t suffix_bytes = input_end - suffix_begin;
                size_t new_res_size = res_data.size() + prefix_bytes + replace_bytes + suffix_bytes + 1; /// +1 for zero terminator
                res_data.resize(new_res_size);

                /// copy prefix before replaced region
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], input_begin, prefix_bytes);
                res_offset += prefix_bytes;

                /// copy replace
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], &replace_data[replace_offset], replace_bytes);
                res_offset += replace_bytes;

                /// copy suffix after replaced region. It is not necessary to copy if suffix_bytes is zero
                if (suffix_bytes)
                {
                    memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], suffix_begin, suffix_bytes);
                    res_offset += suffix_bytes;
                }
            }

            /// add zero terminator
            res_data[res_offset] = 0;
            ++res_offset;
            res_offsets[i] = res_offset;
        }
    }

    template <bool has_three_args, bool offset_is_const, bool length_is_const>
    void vectorVector(
        size_t rows,
        const ColumnString::Chars & input_data,
        const ColumnString::Offsets & input_offsets,
        const ColumnString::Chars & replace_data,
        const ColumnString::Offsets & replace_offsets,
        const ColumnPtr & column_offset,
        const ColumnPtr & column_length,
        Int64 const_offset,
        Int64 const_length,
        ColumnString::Chars & res_data,
        ColumnString::Offsets & res_offsets) const
    {
        if (!has_three_args && length_is_const && const_length < 0)
        {
            vectorVector<true, offset_is_const, false>(
                rows,
                input_data,
                input_offsets,
                replace_data,
                replace_offsets,
                column_offset,
                column_length,
                const_offset,
                -1,
                res_data,
                res_offsets);
            return;
        }

        Int64 length = 0; // maybe negative
        size_t valid_length = 0; // not negative
        if constexpr (!has_three_args && length_is_const)
        {
            assert(const_length >= 0);
            valid_length = const_length;
        }

        Int64 offset = 0; // start from 1, maybe negative
        size_t valid_offset = 0; // start from 0, not negative
        size_t res_offset = 0;
        for (size_t i = 0; i < rows; ++i)
        {
            size_t input_offset = input_offsets[i - 1];
            size_t input_bytes = input_offsets[i] - input_offsets[i - 1] - 1;
            size_t input_size = getSliceSize(&input_data[input_offset], input_bytes);

            size_t replace_offset = replace_offsets[i - 1];
            size_t replace_bytes = replace_offsets[i] - replace_offsets[i - 1] - 1;
            size_t replace_size = getSliceSize(&replace_data[replace_offset], replace_bytes);

            if constexpr (offset_is_const)
            {
                valid_offset = getValidOffset(const_offset, input_size);
            }
            else
            {
                offset = column_offset->getInt(i);
                valid_offset = getValidOffset(offset, input_size);
            }

            if constexpr (has_three_args)
            {
                valid_length = replace_size;
            }
            else if constexpr (!length_is_const)
            {
                length = column_length->getInt(i);
                valid_length = length >= 0 ? length : replace_size;
            }

            size_t prefix_size = valid_offset;
            size_t suffix_size = prefix_size + valid_length > input_size ? 0 : input_size - prefix_size - valid_length;

            if constexpr (!is_utf8)
            {
                size_t new_res_size = res_data.size() + prefix_size + replace_size + suffix_size + 1; /// +1 for zero terminator
                res_data.resize(new_res_size);

                /// copy prefix before replaced region
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], &input_data[input_offset], prefix_size);
                res_offset += prefix_size;

                /// copy replace
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], &replace_data[replace_offset], replace_size);
                res_offset += replace_size;

                /// copy suffix after replaced region. It is not necessary to copy if suffix_size is zero.
                if (suffix_size)
                {
                    memcpySmallAllowReadWriteOverflow15(
                        &res_data[res_offset], &input_data[input_offset + prefix_size + valid_length], suffix_size);
                    res_offset += suffix_size;
                }
            }
            else
            {
                const auto * input_begin = &input_data[input_offset];
                const auto * input_end = &input_data[input_offset + input_bytes];
                const auto * prefix_end = GatherUtils::UTF8StringSource::skipCodePointsForward(input_begin, prefix_size, input_end);
                size_t prefix_bytes = prefix_end > input_end ? input_bytes : prefix_end - input_begin;
                const auto * suffix_begin = GatherUtils::UTF8StringSource::skipCodePointsBackward(input_end, suffix_size, input_begin);
                size_t suffix_bytes = input_end - suffix_begin;
                size_t new_res_size = res_data.size() + prefix_bytes + replace_bytes + suffix_bytes + 1; /// +1 for zero terminator
                res_data.resize(new_res_size);

                /// copy prefix before replaced region
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], input_begin, prefix_bytes);
                res_offset += prefix_bytes;

                /// copy replace
                memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], &replace_data[replace_offset], replace_bytes);
                res_offset += replace_bytes;

                /// copy suffix after replaced region. It is not necessary to copy if suffix_bytes is zero.
                if (suffix_bytes)
                {
                    memcpySmallAllowReadWriteOverflow15(&res_data[res_offset], suffix_begin, suffix_bytes);
                    res_offset += suffix_bytes;
                }
            }

            /// add zero terminator
            res_data[res_offset] = 0;
            ++res_offset;
            res_offsets[i] = res_offset;
        }
    }
};

}

REGISTER_FUNCTION(Overlay)
{
    factory.registerFunction<FunctionOverlay<false>>(
        {.description = R"(
Replace a part of a string `s` with another string `replace`, starting at 1-based index `offset`. By default, the number of bytes removed from `s` equals the length of `replace`. If `length` (the optional fourth argument) is specified, a different number of bytes is removed.
)",
         .categories{"String"}},
        FunctionFactory::Case::Insensitive);

    factory.registerFunction<FunctionOverlay<true>>(
        {.description = R"(
Replace a part of a string `s` with another string `replace`, starting at 1-based index `offset`. By default, the number of bytes removed from `s` equals the length of `replace`. If `length` (the optional fourth argument) is specified, a different number of bytes is removed.

Assumes that the string contains valid UTF-8 encoded text. If this assumption is violated, no exception is thrown and the result is undefined.
)",
         .categories{"String"}},
        FunctionFactory::Case::Sensitive);
}
}
