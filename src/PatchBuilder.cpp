// Copyright (C) 2018 Brick
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "PatchBuilder.h"
#include "BinaryViewAssociatedDataStore.h"

#include <unordered_map>

static const std::string METADATA_KEY = "OBFU_PATCHES";
static const std::string METADATA_VERSION = "0.0.0";

#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/traits/vector.h>
#include <bitsery/flexible.h>
#include <bitsery/flexible/unordered_map.h>

namespace PatchBuilder
{
    struct PatchCollection
    {
        std::unordered_map<uintptr_t, Patch> m_Patches;
        mutable std::mutex m_Mutex;

        void AddPatch(uintptr_t address, Patch patch);
        const Patch* GetPatch(uintptr_t address) const;

        void Save(BinaryView& view);
        void Load(BinaryView& view);
    };

    BinaryViewAssociatedDataStore<PatchCollection> PatchStore;

    PatchCollection* GetPatchCollection(BNBinaryView* view)
    {
        if (PatchCollection* patches = PatchStore.Get(view))
        {
            return patches;
        }

        std::unique_ptr<PatchCollection> patches(new PatchCollection());

        Ref<BinaryView> ref_view = new BinaryView(view);

        patches->Load(*ref_view);

        PatchStore.Set(view, std::move(patches));

        return PatchStore.Get(view);
    }

    void AddPatch(BinaryView& view, uintptr_t address, Patch patch)
    {
        PatchCollection* patches = GetPatchCollection(view.m_object);
        
        patches->AddPatch(address, std::move(patch));
    }

    const Patch* GetPatch(LowLevelILFunction& il, uintptr_t address)
    {
        BNFunction* function = BNGetLowLevelILOwnerFunction(il.m_object);

        if (function == nullptr)
        {
            return nullptr;
        }

        BNBinaryView* view = BNGetFunctionData(function);

        if (view == nullptr)
        {
            return nullptr;
        }

        const PatchCollection* patches = GetPatchCollection(view);

        if (patches == nullptr)
        {
            return nullptr;
        }

        return patches->GetPatch(address);
    }

    void SavePatches(BinaryView & view)
    {
        PatchCollection* patches = GetPatchCollection(view.m_object);

        patches->Save(view);
    }

    void PatchCollection::AddPatch(uintptr_t address, Patch patch)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);

        m_Patches.emplace(address, std::move(patch));
    }

    const Patch* PatchCollection::GetPatch(uintptr_t address) const
    {
        std::lock_guard<std::mutex> guard(m_Mutex);

        auto find = m_Patches.find(address);

        if (find != m_Patches.end())
        {
            return &find->second;
        }

        return nullptr;
    }

    using namespace bitsery;

    using Buffer = std::vector<uint8_t>;
    using OutputAdapter = OutputBufferAdapter<Buffer>;
    using InputAdapter = InputBufferAdapter<Buffer>;

    template <typename S>
    void serialize(S& s, Token& o)
    {
        s.value1b(reinterpret_cast<std::underlying_type_t<TokenType>&>(o.Type));
        s.value8b(o.Value);
    };

    template <typename S>
    void serialize(S& s, Patch& o)
    {
        s.value8b(o.Size);
        s.container(o.Tokens, 4096);
    };

    void PatchCollection::Save(BinaryView& view)
    {
        Buffer b;

        quickSerialization<OutputAdapter>(b, m_Patches);

        DataBuffer db(b.data(), b.size());
        db.ZlibCompress(db);
        b = std::vector<uint8_t>{ &db[0], &db[db.GetLength()] };

        Ref<Metadata> patches = new Metadata({
            { "version", new Metadata(METADATA_VERSION) },
            { "data", new Metadata(b) }
        });

        view.StoreMetadata(METADATA_KEY, patches);
    }

    void PatchCollection::Load(BinaryView& view)
    {
        Ref<Metadata> patches = view.QueryMetadata(METADATA_KEY);

        if (patches && patches->IsKeyValueStore())
        {
            std::map<std::string, Ref<Metadata>> data = patches->GetKeyValueStore();

            if (data.at("version")->GetString() == METADATA_VERSION)
            {
                Buffer b = data.at("data")->GetRaw();

                DataBuffer db(b.data(), b.size());
                db.ZlibDecompress(db);
                b = std::vector<uint8_t>{ &db[0], &db[db.GetLength()] };

                quickDeserialization<InputAdapter>({ b.begin(), b.end() }, m_Patches);
            }
        }
    }

    bool Patch::Evaluate(LowLevelILFunction& il) const
    {
        std::vector<size_t> operands;

        for (const Token& token : Tokens)
        {
            switch (token.Type)
            {
                case TokenType::Instruction:
                {
                    BNLowLevelILOperation operation = static_cast<BNLowLevelILOperation>(token.Value);

                    if (operands.size() < 3)
                    {
                        BinjaLog(ErrorLog, "Missing Instruction Operands (expected 3, got {0})", operands.size());

                        return false;
                    }

                    size_t size = operands.back();
                    operands.pop_back();

                    uint32_t flags = static_cast<uint32_t>(operands.back());
                    operands.pop_back();

                    size_t operand_count = operands.back();
                    operands.pop_back();

                    size_t expected_operand_count = LowLevelILInstruction::operationOperandUsage.at(operation).size();

                    if (expected_operand_count != operand_count)
                    {
                        BinjaLog(ErrorLog, "Mismatched operand count (expected {0}, got {1})", expected_operand_count, operand_count);

                        return false;
                    }

                    if (operands.size() < operand_count)
                    {
                        BinjaLog(ErrorLog, "Missing Exprs (expected {0}, got {1})", operand_count, operands.size());

                        return false;
                    }

                    size_t exprs[4]{};
                    auto expr_iter = operands.end() - operand_count;
                    std::copy_n(expr_iter, operand_count, exprs);
                    operands.erase(expr_iter, operands.end());

                    ExprId expr = il.AddExpr(operation, size, flags,
                        static_cast<ExprId>(exprs[0]),
                        static_cast<ExprId>(exprs[1]),
                        static_cast<ExprId>(exprs[2]),
                        static_cast<ExprId>(exprs[3])
                    );

                    operands.push_back(static_cast<size_t>(expr));

                } break;

                case TokenType::Operand:
                {
                    operands.push_back(token.Value);
                } break;

                default:
                {
                    BinjaLog(ErrorLog, "Bad Token: {0} - {1}", static_cast<int>(token.Type), token.Value);

                    return false;
                } break;
            }
        }

        for (size_t expr : operands)
        {
            il.AddInstruction(static_cast<ExprId>(expr));
        }

        return true;
    }
}
