#include "core/DevBuildCache.h"
#include "core/VolumeIndex.h"

#include <gtest/gtest.h>

#include <string>

namespace minisys {

// ---- v2.5 dev build-artifact classification --------------------------------

TEST(DevBuildCache, SafeArtifactNames) {
    EXPECT_TRUE(DevBuildCache::IsSafeArtifactName(L"bin"));
    EXPECT_TRUE(DevBuildCache::IsSafeArtifactName(L"Obj"));
    EXPECT_TRUE(DevBuildCache::IsSafeArtifactName(L".VS"));
    EXPECT_TRUE(DevBuildCache::IsSafeArtifactName(L"ipch"));
    EXPECT_TRUE(DevBuildCache::IsSafeArtifactName(L"x64"));
    EXPECT_TRUE(DevBuildCache::IsSafeArtifactName(L"Debug"));
    EXPECT_TRUE(DevBuildCache::IsSafeArtifactName(L"Release"));
    EXPECT_FALSE(DevBuildCache::IsSafeArtifactName(L"bin2"));
    EXPECT_FALSE(DevBuildCache::IsSafeArtifactName(L".vscode"));  // config, not output
    EXPECT_FALSE(DevBuildCache::IsSafeArtifactName(L"source"));
    EXPECT_FALSE(DevBuildCache::IsSafeArtifactName(L""));
}

TEST(DevBuildCache, CautiousArtifactNames) {
    EXPECT_TRUE(DevBuildCache::IsCautiousArtifactName(L"build"));
    EXPECT_TRUE(DevBuildCache::IsCautiousArtifactName(L"BUILD"));
    EXPECT_TRUE(DevBuildCache::IsCautiousArtifactName(L"node_modules"));
    EXPECT_TRUE(DevBuildCache::IsCautiousArtifactName(L"cmake-build-debug"));
    EXPECT_TRUE(DevBuildCache::IsCautiousArtifactName(L"target"));
    EXPECT_FALSE(DevBuildCache::IsCautiousArtifactName(L"bin"));
    EXPECT_FALSE(DevBuildCache::IsCautiousArtifactName(L"builder"));
}

TEST(DevBuildCache, ProjectMarkerNames) {
    EXPECT_TRUE(DevBuildCache::IsProjectMarkerName(L"a.sln"));
    EXPECT_TRUE(DevBuildCache::IsProjectMarkerName(L"My.VCXPROJ"));
    EXPECT_TRUE(DevBuildCache::IsProjectMarkerName(L"p.csproj"));
    EXPECT_TRUE(DevBuildCache::IsProjectMarkerName(L"CMakeLists.txt"));
    EXPECT_TRUE(DevBuildCache::IsProjectMarkerName(L"makefile"));
    EXPECT_TRUE(DevBuildCache::IsProjectMarkerName(L"package.json"));
    EXPECT_TRUE(DevBuildCache::IsProjectMarkerName(L"Cargo.toml"));
    EXPECT_FALSE(DevBuildCache::IsProjectMarkerName(L"main.cpp"));
    EXPECT_FALSE(DevBuildCache::IsProjectMarkerName(L"sln.txt"));
    EXPECT_FALSE(DevBuildCache::IsProjectMarkerName(L".sln"));   // no basename
}

// ---- v2.5 Everything-style in-query search filters --------------------------

TEST(SearchFilterParsing, MixedTokens) {
    std::wstring free;
    auto f = VolumeIndex::ParseFilterTerms(L"foo folder: ext:cpp;h bar", free);
    EXPECT_TRUE(f.dirsOnly);
    EXPECT_FALSE(f.filesOnly);
    ASSERT_EQ(f.exts.size(), 2u);
    EXPECT_EQ(f.exts[0], L"cpp");
    EXPECT_EQ(f.exts[1], L"h");
    EXPECT_EQ(free, L"foo bar");
}

TEST(SearchFilterParsing, DotExtAndIsAliases) {
    std::wstring free;
    auto f = VolumeIndex::ParseFilterTerms(L"file: is:folder ext:.md,txt", free);
    EXPECT_TRUE(f.filesOnly);
    EXPECT_TRUE(f.dirsOnly);
    ASSERT_EQ(f.exts.size(), 2u);
    EXPECT_EQ(f.exts[0], L"md");
    EXPECT_EQ(f.exts[1], L"txt");
    EXPECT_TRUE(free.empty());
}

TEST(SearchFilterParsing, NoFilters) {
    std::wstring free;
    auto f = VolumeIndex::ParseFilterTerms(L"just words", free);
    EXPECT_FALSE(f.dirsOnly);
    EXPECT_FALSE(f.filesOnly);
    EXPECT_TRUE(f.exts.empty());
    EXPECT_EQ(free, L"just words");
}

// ---- v2.6 quick-filter token composition ------------------------------------

TEST(QueryToggle, ReplacesSameCategory) {
    EXPECT_EQ(VolumeIndex::ToggleQueryToken(L"foo ext:pdb", L"ext:obj"),
              L"foo ext:obj");
    EXPECT_EQ(VolumeIndex::ToggleQueryToken(L"ext:pdb", L"ext:zip;rar"),
              L"ext:zip;rar");
}

TEST(QueryToggle, KeepsOtherCategoriesAndFreeText) {
    EXPECT_EQ(VolumeIndex::ToggleQueryToken(L"a ext:pdb b folder:x", L"ext:ilk"),
              L"a b folder:x ext:ilk");
    EXPECT_EQ(VolumeIndex::ToggleQueryToken(L"notes folder:debug", L"ext:pch"),
              L"notes folder:debug ext:pch");
}

TEST(QueryToggle, EmptyTokenIsNoOp) {
    EXPECT_EQ(VolumeIndex::ToggleQueryToken(L"ext:pdb foo", L""),
              L"ext:pdb foo");
    // Removing a category is done by selecting a same-category token of
    // the desired value, or "清除全部过滤" (ParseFilterTerms) in the UI.
}

TEST(QueryToggle, AppendsWhenAbsent) {
    EXPECT_EQ(VolumeIndex::ToggleQueryToken(L"", L"folder:debug"),
              L"folder:debug");
    EXPECT_EQ(VolumeIndex::ToggleQueryToken(L"readme", L"file:"),
              L"readme file:");
}

TEST(SearchFilters, AppliedDuringScan) {
    auto& vi = VolumeIndex::Instance();
    vi.ResetForTesting();
    vi.AddNodeForTesting(1, 1, L"", true, 0);            // root (self-parent)
    vi.AddNodeForTesting(2, 1, L"docs", true, 0);
    vi.AddNodeForTesting(3, 2, L"a.cpp", false, 0);
    vi.AddNodeForTesting(4, 2, L"readme.md", false, 0);
    vi.AddNodeForTesting(5, 1, L"build", true, 0);
    vi.FinalizeForTesting(L'Z');

    auto count = [&vi](const VolumeIndex::SearchFilter& f,
                       const std::wstring& q = L"") {
        size_t n = 0;
        vi.Search(q, false, 100,
                  [&](const VolumeIndex::SearchHit&) { ++n; return true; }, f);
        return n;
    };

    VolumeIndex::SearchFilter ff;
    ff.filesOnly = true;
    EXPECT_EQ(count(ff), 2u);                       // a.cpp + readme.md

    VolumeIndex::SearchFilter fd;
    fd.dirsOnly = true;
    EXPECT_EQ(count(fd), 2u);                       // docs + build

    VolumeIndex::SearchFilter fe;
    fe.exts = { L"md" };
    EXPECT_EQ(count(fe), 1u);                       // readme.md only

    // Filter with free terms still ANDs the terms.
    VolumeIndex::SearchFilter fq;
    fq.filesOnly = true;
    EXPECT_EQ(count(fq, L"readme"), 1u);
    EXPECT_EQ(count(fq, L"nothing-matches"), 0u);

    vi.ResetForTesting();
}

} // namespace minisys
