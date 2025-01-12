// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef DORIS_BE_SRC_OLAP_ROW_BLOCK_H
#define DORIS_BE_SRC_OLAP_ROW_BLOCK_H

#include <exception>
#include <iterator>
#include <vector>

#include "gen_cpp/olap_file.pb.h"
#include "olap/olap_common.h"
#include "olap/olap_define.h"
#include "olap/row_cursor.h"
#include "olap/utils.h"
#include "runtime/vectorized_row_batch.h"

namespace doris {

class ExprContext;

struct RowBlockInfo {
    RowBlockInfo() : checksum(0), row_num(0) { }
    RowBlockInfo(uint32_t value, uint32_t num) : checksum(value), row_num(num) { }

    uint32_t checksum;
    uint32_t row_num;       // block最大数据行数
    bool null_supported;
    std::vector<uint32_t> column_ids;
};

// 一般由256或512行组成一个RowBlock。
// RowBlock类有如下职责：
// 1. 外界从磁盘上读取未解压数据，用decompress函数传给RowBlock，解压后的数据保存在
// RowBlock的内部buf中；
// 2. 给定row_index，读取内部各field的值
// 3. 给定查询的key，在RowBlock内做二分查找，返回起点的行偏移；
// 4. 向量化的条件过滤下推到RowBlock级别进行，因此增加完成过滤的数据读取借口
class RowBlock {
    // Please keep these classes as 'friend'.  They have to use lots of private fields for
    // faster operation.
    friend class RowBlockChanger;
    friend class VectorizedRowBatch;
public:
    RowBlock(const TabletSchema* schema);

    // 注意回收内部buffer
    ~RowBlock();

    // row_num是RowBlock的最大行数，fields为了初始化各个field的起始位置。
    // 在field都为定长的情况下根据这两个值可以确定RowBlock内部buffer的大小，
    // 目前只考虑定长，因此在函数可以分配内存资源。
    OLAPStatus init(const RowBlockInfo& block_info);

    inline void get_row(uint32_t row_index, RowCursor* cursor) const {
        cursor->attach(_mem_buf + row_index * _mem_row_bytes);
    }

    template<typename RowType>
    inline void set_row(uint32_t row_index, const RowType& row) const {
        memcpy(_mem_buf + row_index * _mem_row_bytes, row.row_ptr(), _mem_row_bytes);
    }

    // called when finished fill this row_block
    OLAPStatus finalize(uint32_t row_num);

    // 根据key的值在RowBlock内部做二分查找，返回第一条对应的row_index，
    // find_last为false，找到的lowerbound，反之对应的upperbound。
    // _is_use_vectorized为true，则在经过向量化条件过滤的VectorizedRowBatch中查找，反之使用原始数据
    OLAPStatus find_row(const RowCursor& key, 
                        bool find_last, 
                        uint32_t* row_index) const;

    const uint32_t row_num() const { return _info.row_num; }
    const RowBlockInfo& row_block_info() const { return _info; }
    const TabletSchema& tablet_schema() const { return *_schema; }
    size_t capacity() const { return _capacity; }

    // Return field pointer, this pointer point to the nullbyte before the field
    // layout is nullbyte|Field
    inline char* field_ptr(size_t row, size_t col) const {
        return _mem_buf + _mem_row_bytes * row + _field_offset_in_memory[col];
    }

    MemPool* mem_pool() const {
        return _mem_pool.get();
    }

    // 重用rowblock之前需调用clear，恢复到init之后的原始状态
    void clear();

    size_t pos() const { return _pos; }
    void set_pos(size_t pos) { _pos = pos; }
    void pos_inc() { _pos++; }
    size_t limit() const { return _limit; }
    void set_limit(size_t limit) { _limit = limit; }
    size_t remaining() const { return _limit - _pos; }
    bool has_remaining() const { return _pos < _limit; }
    uint8_t block_status() const { return _block_status; }
    void set_block_status(uint8_t status) { _block_status = status; }

private:
    // 仿函数里，根据iterator的operator*返回的序号获取数据结构的值，
    // 与待比较的值完成比较，less就是小于函数
    class RowBlockComparator {
    public:
        RowBlockComparator(const RowBlock* container, 
                           RowCursor* helper_cursor) :
                _container(container),
                _helper_cursor(helper_cursor) {}
        ~RowBlockComparator() {}
        
        // less comparator
        bool operator()(const iterator_offset_t& index, const RowCursor& key) const {
            return _compare(index, key, COMPARATOR_LESS);
        }
        // larger comparator
        bool operator()(const RowCursor& key, const iterator_offset_t& index) const {
            return _compare(index, key, COMPARATOR_LARGER);
        }

    private:
        bool _compare(const iterator_offset_t& index,
                      const RowCursor& key,
                      ComparatorEnum comparator_enum) const {
            _container->get_row(index, _helper_cursor);
            if (comparator_enum == COMPARATOR_LESS) {
                return _helper_cursor->cmp(key) < 0;
            } else {
                return _helper_cursor->cmp(key) > 0;
            }
        }

        const RowBlock* _container;
        RowCursor* _helper_cursor;
    };

    bool has_nullbyte() {
        return _null_supported;
    }

    // Compute layout for storage buffer and  memory buffer
    void _compute_layout();

    uint32_t _capacity;
    RowBlockInfo _info;
    const TabletSchema* _schema;     // 内部保存的schema句柄
    
    bool _null_supported;

    // Data in memory is construct from row cursors, these row cursors's size is equal
    char* _mem_buf = nullptr;
    // equal with _mem_row_bytes * _info.row_num
    size_t _mem_buf_bytes = 0;
    // row's size in bytes, in one block, all rows's size is equal
    size_t _mem_row_bytes = 0;

    // Field offset of memory row format, used to get field ptr in memory row
    std::vector<size_t> _field_offset_in_memory;

    // Data in storage will be construct of two parts: fixed-length field stored in ahead
    // of buffer; content of variable length field(Varchar/HLL) are stored after first part

    // only used for SegmentReader to covert VectorizedRowBatch to RowBlock
    // Be careful to use this
    size_t _pos = 0;
    size_t _limit = 0;
    uint8_t _block_status = DEL_PARTIAL_SATISFIED;

    std::unique_ptr<MemTracker> _tracker;
    std::unique_ptr<MemPool> _mem_pool;
    // 由于内部持有内存资源，所以这里禁止拷贝和赋值
    DISALLOW_COPY_AND_ASSIGN(RowBlock);
};

}  // namespace doris

#endif // DORIS_BE_SRC_OLAP_ROW_BLOCK_H
