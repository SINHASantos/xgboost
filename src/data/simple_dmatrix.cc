/**
 * Copyright 2014-2025, XGBoost Contributors
 * \file simple_dmatrix.cc
 * \brief the input data structure for gradient boosting
 * \author Tianqi Chen
 */
#include "simple_dmatrix.h"

#include <algorithm>
#include <limits>
#include <numeric>  // for accumulate
#include <type_traits>
#include <vector>

#include "../collective/allgather.h"
#include "../collective/communicator-inl.h"  // for GetWorldSize, GetRank, Allgather
#include "../common/error_msg.h"             // for InconsistentMaxBin
#include "./simple_batch_iterator.h"
#include "adapter.h"
#include "batch_utils.h"    // for CheckEmpty, RegenGHist
#include "cat_container.h"  // for CatContainer
#include "ellpack_page.h"   // for EllpackPage
#include "gradient_index.h"
#include "xgboost/c_api.h"
#include "xgboost/data.h"

namespace xgboost::data {
MetaInfo& SimpleDMatrix::Info() { return info_; }

const MetaInfo& SimpleDMatrix::Info() const { return info_; }

DMatrix* SimpleDMatrix::Slice(common::Span<int32_t const> ridxs) {
  auto out = new SimpleDMatrix;
  SparsePage& out_page = *out->sparse_page_;
  // Convert to uint64 to avoid a breaking change in the C API. The performance impact is
  // small since we have to iteratve through the sparse page.
  std::vector<bst_idx_t> h_ridx(ridxs.data(), ridxs.data() + ridxs.size());
  for (auto const& page : this->GetBatches<SparsePage>()) {
    auto batch = page.GetView();
    auto& h_data = out_page.data.HostVector();
    auto& h_offset = out_page.offset.HostVector();
    size_t rptr{0};
    for (auto ridx : ridxs) {
      auto inst = batch[ridx];
      rptr += inst.size();
      std::copy(inst.begin(), inst.end(), std::back_inserter(h_data));
      h_offset.emplace_back(rptr);
    }
    auto ctx = this->fmat_ctx_.MakeCPU();
    out->Info() = this->Info().Slice(&ctx, h_ridx, h_offset.back());
  }
  out->fmat_ctx_ = this->fmat_ctx_;

  out->Info().Cats()->Copy(&fmat_ctx_, *this->Info().Cats());
  return out;
}

DMatrix* SimpleDMatrix::SliceCol(int num_slices, int slice_id) {
  if (this->Cats()->HasCategorical()) {
    LOG(FATAL) << "Slicing column is not supported for DataFrames with categorical columns.";
  }
  auto out = new SimpleDMatrix;
  SparsePage& out_page = *out->sparse_page_;
  auto const slice_size = info_.num_col_ / num_slices;
  auto const slice_start = slice_size * slice_id;
  auto const slice_end = (slice_id == num_slices - 1) ? info_.num_col_ : slice_start + slice_size;
  for (auto const& page : this->GetBatches<SparsePage>()) {
    auto batch = page.GetView();
    auto& h_data = out_page.data.HostVector();
    auto& h_offset = out_page.offset.HostVector();
    size_t rptr{0};
    for (bst_idx_t i = 0; i < this->Info().num_row_; i++) {
      auto inst = batch[i];
      auto prev_size = h_data.size();
      std::copy_if(inst.begin(), inst.end(), std::back_inserter(h_data),
                   [&](Entry e) { return e.index >= slice_start && e.index < slice_end; });
      rptr += h_data.size() - prev_size;
      h_offset.emplace_back(rptr);
    }
    out->Info() = this->Info().Copy();
    out->Info().num_nonzero_ = h_offset.back();
  }
  out->Info().data_split_mode = DataSplitMode::kCol;
  return out;
}

void SimpleDMatrix::ReindexFeatures(Context const* ctx, DataSplitMode split_mode) {
  if (split_mode == DataSplitMode::kCol && collective::GetWorldSize() > 1) {
    std::vector<std::uint64_t> buffer(collective::GetWorldSize());
    buffer[collective::GetRank()] = this->info_.num_col_;
    auto rc = collective::Allgather(ctx, linalg::MakeVec(buffer.data(), buffer.size()));
    SafeColl(rc);
    auto offset = std::accumulate(buffer.cbegin(), buffer.cbegin() + collective::GetRank(), 0);
    if (offset == 0) {
      return;
    }
    sparse_page_->Reindex(offset, ctx->Threads());
  }
}

BatchSet<SparsePage> SimpleDMatrix::GetRowBatches() {
  // since csr is the default data structure so `source_` is always available.
  auto begin_iter =
      BatchIterator<SparsePage>(new SimpleBatchIteratorImpl<SparsePage>(sparse_page_));
  return BatchSet<SparsePage>(begin_iter);
}

BatchSet<CSCPage> SimpleDMatrix::GetColumnBatches(Context const* ctx) {
  // column page doesn't exist, generate it
  if (!column_page_) {
    auto n = std::numeric_limits<decltype(Entry::index)>::max();
    if (this->sparse_page_->Size() > n) {
      error::MaxSampleSize(n);
    }
    column_page_.reset(new CSCPage(sparse_page_->GetTranspose(info_.num_col_, ctx->Threads())));
  }
  auto begin_iter = BatchIterator<CSCPage>(new SimpleBatchIteratorImpl<CSCPage>(column_page_));
  return BatchSet<CSCPage>(begin_iter);
}

BatchSet<SortedCSCPage> SimpleDMatrix::GetSortedColumnBatches(Context const* ctx) {
  // Sorted column page doesn't exist, generate it
  if (!sorted_column_page_) {
    auto n = std::numeric_limits<decltype(Entry::index)>::max();
    if (this->sparse_page_->Size() > n) {
      error::MaxSampleSize(n);
    }
    sorted_column_page_.reset(
        new SortedCSCPage(sparse_page_->GetTranspose(info_.num_col_, ctx->Threads())));
    sorted_column_page_->SortRows(ctx->Threads());
  }
  auto begin_iter =
      BatchIterator<SortedCSCPage>(new SimpleBatchIteratorImpl<SortedCSCPage>(sorted_column_page_));
  return BatchSet<SortedCSCPage>(begin_iter);
}

BatchSet<EllpackPage> SimpleDMatrix::GetEllpackBatches(Context const* ctx,
                                                       const BatchParam& param) {
  detail::CheckEmpty(batch_param_, param);
  if (ellpack_page_ && param.Initialized() && param.forbid_regen) {
    if (detail::RegenGHist(batch_param_, param)) {
      CHECK_EQ(batch_param_.max_bin, param.max_bin) << error::InconsistentMaxBin();
    }
    CHECK(!detail::RegenGHist(batch_param_, param));
  }
  if (!ellpack_page_ || detail::RegenGHist(batch_param_, param)) {
    // ELLPACK page doesn't exist, generate it
    LOG(INFO) << "Generating new Ellpack page.";
    // These places can ask for a ellpack page:
    // - GPU hist: the ctx must be on CUDA.
    // - IterativeDMatrix::InitFromCUDA: The ctx must be on CUDA.
    // - IterativeDMatrix::InitFromCPU: It asks for ellpack only if it exists. It should
    //   not regen, otherwise it indicates a mismatched parameter like max_bin.
    CHECK_GE(param.max_bin, 2);
    if (ctx->IsCUDA()) {
      // The context passed in is on GPU, we pick it first since we prioritize the context
      // in Booster.
      ellpack_page_.reset(new EllpackPage(ctx, this, param));
    } else if (fmat_ctx_.IsCUDA()) {
      // DMatrix was initialized on GPU, we use the context from initialization.
      ellpack_page_.reset(new EllpackPage(&fmat_ctx_, this, param));
    } else {
      // Mismatched parameter, user set a new max_bin during training.
      auto cuda_ctx = ctx->MakeCUDA();
      ellpack_page_.reset(new EllpackPage(&cuda_ctx, this, param));
    }

    batch_param_ = param.MakeCache();
  }
  auto begin_iter =
      BatchIterator<EllpackPage>(new SimpleBatchIteratorImpl<EllpackPage>(ellpack_page_));
  return BatchSet<EllpackPage>(begin_iter);
}

BatchSet<GHistIndexMatrix> SimpleDMatrix::GetGradientIndex(Context const* ctx,
                                                           const BatchParam& param) {
  detail::CheckEmpty(batch_param_, param);
  // Check whether we can regenerate the gradient index. This is to keep the consistency
  // between evaluation data and training data.
  if (gradient_index_ && param.Initialized() && param.forbid_regen) {
    if (detail::RegenGHist(batch_param_, param)) {
      CHECK_EQ(batch_param_.max_bin, param.max_bin) << error::InconsistentMaxBin();
    }
    CHECK(!detail::RegenGHist(batch_param_, param)) << "Inconsistent sparse threshold.";
  }
  if (!gradient_index_ || detail::RegenGHist(batch_param_, param)) {
    // GIDX page doesn't exist, generate it
    LOG(DEBUG) << "Generating new Gradient Index.";
    // These places can ask for a CSR gidx:
    // - CPU Hist: the ctx must be on CPU.
    // - IterativeDMatrix::InitFromCPU: The ctx must be on CPU.
    // - IterativeDMatrix::InitFromCUDA: It asks for gidx only if it exists. It should not
    //   regen, otherwise it indicates a mismatched parameter like max_bin.
    CHECK_GE(param.max_bin, 2);
    // Used only by approx.
    auto sorted_sketch = param.regen;
    if (!ctx->IsCUDA()) {
      // The context passed in is on CPU, we pick it first since we prioritize the context
      // in Booster.
      gradient_index_.reset(new GHistIndexMatrix{ctx, this, param.max_bin, param.sparse_thresh,
                                                 sorted_sketch, param.hess});
    } else if (!fmat_ctx_.IsCUDA()) {
      // DMatrix was initialized on CPU, we use the context from initialization.
      gradient_index_.reset(new GHistIndexMatrix{&fmat_ctx_, this, param.max_bin,
                                                 param.sparse_thresh, sorted_sketch, param.hess});
    } else {
      // Mismatched parameter, user set a new max_bin during training.
      auto cpu_ctx = ctx->MakeCPU();
      gradient_index_.reset(new GHistIndexMatrix{&cpu_ctx, this, param.max_bin, param.sparse_thresh,
                                                 sorted_sketch, param.hess});
    }

    batch_param_ = param.MakeCache();
    CHECK_EQ(batch_param_.hess.data(), param.hess.data());
  }
  auto begin_iter = BatchIterator<GHistIndexMatrix>(
      new SimpleBatchIteratorImpl<GHistIndexMatrix>(gradient_index_));
  return BatchSet<GHistIndexMatrix>(begin_iter);
}

BatchSet<ExtSparsePage> SimpleDMatrix::GetExtBatches(Context const*, BatchParam const&) {
  auto casted = std::make_shared<ExtSparsePage>(sparse_page_);
  CHECK(casted);
  auto begin_iter =
      BatchIterator<ExtSparsePage>(new SimpleBatchIteratorImpl<ExtSparsePage>(casted));
  return BatchSet<ExtSparsePage>(begin_iter);
}

template <typename AdapterT>
SimpleDMatrix::SimpleDMatrix(AdapterT* adapter, float missing, int nthread,
                             DataSplitMode data_split_mode) {
  Context ctx;
  ctx.Init(Args{{"nthread", std::to_string(nthread)}});

  std::vector<uint64_t> qids;
  uint64_t default_max = std::numeric_limits<uint64_t>::max();
  uint64_t last_group_id = default_max;
  bst_uint group_size = 0;
  auto& offset_vec = sparse_page_->offset.HostVector();
  auto& data_vec = sparse_page_->data.HostVector();
  uint64_t inferred_num_columns = 0;
  uint64_t total_batch_size = 0;
  // batch_size is either number of rows or cols, depending on data layout

  auto push_page = [&](auto const& batch) {
    if constexpr (std::is_same_v<AdapterT, ColumnarAdapter>) {
      if (adapter->HasRefCategorical()) {
        auto [enc_batch, mapping] = MakeEncColumnarBatch(&ctx, adapter);
        return sparse_page_->Push(enc_batch, missing, ctx.Threads());
      }
    }
    return sparse_page_->Push(batch, missing, ctx.Threads());
  };

  adapter->BeforeFirst();
  // Iterate over batches of input data
  while (adapter->Next()) {
    auto& batch = adapter->Value();
    bst_idx_t batch_max_columns = push_page(batch);
    inferred_num_columns = std::max(batch_max_columns, inferred_num_columns);
    total_batch_size += batch.Size();
    // Append meta information if available
    if (batch.Labels() != nullptr) {
      info_.labels.ModifyInplace([&](auto* data, common::Span<size_t, 2> shape) {
        shape[1] = 1;
        auto& labels = data->HostVector();
        labels.insert(labels.end(), batch.Labels(), batch.Labels() + batch.Size());
        shape[0] += batch.Size();
      });
    }
    if (batch.Weights() != nullptr) {
      auto& weights = info_.weights_.HostVector();
      weights.insert(weights.end(), batch.Weights(), batch.Weights() + batch.Size());
    }
    if (batch.BaseMargin() != nullptr) {
      info_.base_margin_ = decltype(info_.base_margin_){
          batch.BaseMargin(), batch.BaseMargin() + batch.Size(), {batch.Size()}, DeviceOrd::CPU()};
    }
    if (batch.Qid() != nullptr) {
      qids.insert(qids.end(), batch.Qid(), batch.Qid() + batch.Size());
      // get group
      for (size_t i = 0; i < batch.Size(); ++i) {
        const uint64_t cur_group_id = batch.Qid()[i];
        if (last_group_id == default_max || last_group_id != cur_group_id) {
          info_.group_ptr_.push_back(group_size);
        }
        last_group_id = cur_group_id;
        ++group_size;
      }
    }
  }

  if (last_group_id != default_max) {
    if (group_size > info_.group_ptr_.back()) {
      info_.group_ptr_.push_back(group_size);
    }
  }

  // Deal with empty rows/columns if necessary
  if (adapter->NumColumns() == kAdapterUnknownSize) {
    info_.num_col_ = inferred_num_columns;
  } else {
    info_.num_col_ = adapter->NumColumns();
  }

  if constexpr (std::is_same_v<AdapterT, ColumnarAdapter>) {
    if (adapter->HasRefCategorical()) {
      info_.Cats(std::make_shared<CatContainer>(adapter->RefCats()));
    } else if (adapter->HasCategorical()) {
      info_.Cats(std::make_shared<CatContainer>(adapter->Cats()));
    }
  }

  // Must called before sync column
  this->ReindexFeatures(&ctx, data_split_mode);
  this->info_.SynchronizeNumberOfColumns(&ctx, data_split_mode);

  if (adapter->NumRows() == kAdapterUnknownSize) {
    using IteratorAdapterT =
        IteratorAdapter<DataIterHandle, XGBCallbackDataIterNext, XGBoostBatchCSR>;
    // If AdapterT is either IteratorAdapter or FileAdapter type, use the total batch size to
    // determine the correct number of rows, as offset_vec may be too short
    if (std::is_same_v<AdapterT, IteratorAdapterT> || std::is_same_v<AdapterT, FileAdapter>) {
      info_.num_row_ = total_batch_size;
      // Ensure offset_vec.size() - 1 == [number of rows]
      while (offset_vec.size() - 1 < total_batch_size) {
        offset_vec.emplace_back(offset_vec.back());
      }
    } else {
      CHECK((std::is_same_v<AdapterT, CSCArrayAdapter>)) << "Expecting a CSC adapter.";
      info_.num_row_ = offset_vec.size() - 1;
    }
  } else {
    if (offset_vec.empty()) {
      offset_vec.emplace_back(0);
    }
    while (offset_vec.size() - 1 < adapter->NumRows()) {
      offset_vec.emplace_back(offset_vec.back());
    }
    info_.num_row_ = adapter->NumRows();
  }
  info_.num_nonzero_ = data_vec.size();

  // Sort the index for row partitioners used by variuos tree methods.
  if (!sparse_page_->IsIndicesSorted(ctx.Threads())) {
    sparse_page_->SortIndices(ctx.Threads());
  }

  this->fmat_ctx_ = ctx;
}

SimpleDMatrix::SimpleDMatrix(dmlc::Stream* in_stream) {
  int tmagic;
  CHECK(in_stream->Read(&tmagic)) << "invalid input file format";
  CHECK_EQ(tmagic, kMagic) << "invalid format, magic number mismatch";
  info_.LoadBinary(in_stream);
  in_stream->Read(&sparse_page_->offset.HostVector());
  in_stream->Read(&sparse_page_->data.HostVector());
}

void SimpleDMatrix::SaveToLocalFile(const std::string& fname) {
  std::unique_ptr<dmlc::Stream> fo(dmlc::Stream::Create(fname.c_str(), "w"));
  int tmagic = kMagic;
  fo->Write(tmagic);
  info_.SaveBinary(fo.get());
  fo->Write(sparse_page_->offset.HostVector());
  fo->Write(sparse_page_->data.HostVector());
}

#define INSTANTIATE_SDCTOR(__ADAPTER_T)                                                            \
  template SimpleDMatrix::SimpleDMatrix(__ADAPTER_T* adapter, float missing, std::int32_t nthread, \
                                        DataSplitMode data_split_mode);

INSTANTIATE_SDCTOR(DenseAdapter)
INSTANTIATE_SDCTOR(ArrayAdapter)
INSTANTIATE_SDCTOR(CSRArrayAdapter)
INSTANTIATE_SDCTOR(CSCArrayAdapter)
INSTANTIATE_SDCTOR(FileAdapter)
INSTANTIATE_SDCTOR(ColumnarAdapter)
namespace {
using IterAdapterT = IteratorAdapter<DataIterHandle, XGBCallbackDataIterNext, XGBoostBatchCSR>;
}
INSTANTIATE_SDCTOR(IterAdapterT)

#undef INSTANTIATE_SDCTOR
}  // namespace xgboost::data
