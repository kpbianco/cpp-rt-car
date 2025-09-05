#include <gtest/gtest.h>
#include <simcore/sparse_set.hpp>
#include <vector>
#include <cstdint>

TEST(SparseSet, InsertRemoveAligned)
{
    SparseSet<double> pos;
    pos.insert(2, 1.0);
    pos.insert(5, 3.0);

    EXPECT_TRUE(pos.has(2));
    EXPECT_EQ(*pos.get(5), 3.0);

    uintptr_t addr = reinterpret_cast<uintptr_t>(pos.data());
    EXPECT_EQ(addr % 64, 0u);
    EXPECT_GE(pos.capacity() - pos.size(), SparseSet<double>::tail_padding());
}

TEST(SparseSet, GroupView)
{
    SparseSet<double> pos;
    SparseSet<int> tag;
    pos.insert(1, 1.0);
    pos.insert(3, 2.0);
    tag.insert(3, 5);
    tag.insert(4, 7);

    std::vector<std::size_t> ids;
    group_view(pos, tag, [&](std::size_t id, double& p, int& t){
        ids.push_back(id);
        p += 1.0;
        t += 1;
    });

    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 3u);
    EXPECT_DOUBLE_EQ(*pos.get(3), 3.0);
    EXPECT_EQ(*tag.get(3), 6);
}

