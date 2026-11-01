#include <linux/client/window/pixel_buffer.h>
#include <array>
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
int checks=0,failures=0;
void Check(bool value,const char *name)
{
    ++checks;failures+=!value;std::printf("%s: %s\n",value?"PASS":"FAIL",name);
}
bool Invalid(kisak::window::PixelBufferView view)
{
    try { kisak::window::ValidatePixelBuffer(view); }
    catch(const std::invalid_argument&) { return true; }
    return false;
}
}

int main()
{
    using kisak::window::PixelBuffer;
    try {
        Check(!std::is_copy_constructible_v<PixelBuffer> && std::is_nothrow_move_constructible_v<PixelBuffer>,
              "pixel storage has explicit unique movable ownership");
        PixelBuffer original;
        Check(original.Bytes().empty() && Invalid(original.View()),"empty owner is not a presentable frame");
        Check(original.Resize(2,2) && original.Bytes().size()==16,"allocation owns a complete tightly packed frame");
        for(auto byte:original.Bytes())if(byte!=0)throw std::runtime_error("new pixel storage was not zero initialized");
        original.Bytes()[0]=91;original.Bytes()[15]=173;
        const auto *address=original.Bytes().data();
        Check(!original.Resize(2,2) && original.Bytes().data()==address && original.Bytes()[15]==173,
              "same-size resize preserves borrowed storage and content");
        for(auto extent:{std::pair{0,2},std::pair{-1,2},std::pair{2,0},std::pair{8193,1}}) {
            bool rejected=false;
            try { original.Resize(extent.first,extent.second); }
            catch(const std::invalid_argument&) { rejected=true; }
            Check(rejected && original.Extent().width==2 && original.Bytes().data()==address && original.Bytes()[0]==91,
                  "invalid resize preserves prior resource transactionally");
        }
        PixelBuffer moved(std::move(original));
        Check(moved.Bytes().data()==address && moved.Bytes()[15]==173 &&
              original.Extent().width==0 && original.Extent().height==0 && original.Bytes().empty(),
              "move transfers storage and leaves an empty reusable source");
        Check(original.Resize(2,2) && original.Bytes().data()!=moved.Bytes().data() && original.Bytes()[0]==0,
              "moved-from owner can allocate its former dimensions again");
        PixelBuffer assigned;assigned.Resize(3,3);assigned=std::move(moved);
        Check(assigned.Bytes().data()==address && assigned.Bytes()[0]==91 && moved.Bytes().empty() && moved.Extent().width==0,
              "move assignment releases destination storage and empties source");
        auto &alias=assigned;assigned=std::move(alias);
        Check(assigned.Bytes().data()==address && assigned.Extent().height==2 && assigned.Bytes()[15]==173,
              "self move preserves the owned frame");
        Check(assigned.Resize(3,1) && assigned.Bytes().size()==12 && assigned.Bytes()[0]==0,
              "dimension change installs a new cleared resource");

        std::array<uint8_t,20> padded{};
        Check(!Invalid({2,2,12,padded}),"row padding requires only actual pixels on the final row");
        Check(Invalid({2,2,12,std::span<const uint8_t>(padded).first(19)}),"one missing final pixel byte is rejected");
        Check(Invalid({2,2,7,padded}),"pitch smaller than a pixel row is rejected");
        Check(Invalid({1,1,size_t(INT_MAX)+1,padded}),"pitch outside SDL integer range is rejected");
        Check(Invalid({1,8192,size_t(INT_MAX),padded}),"oversized row span is rejected on 32-bit and 64-bit hosts");
        Check(Invalid({1,1,4,{}}),"missing borrowed storage is rejected");
        Check(Invalid({8193,1,32772,padded}),"borrowed dimensions follow the resource bound");
    } catch(const std::exception &error) {
        std::fprintf(stderr,"pixel resource test failed: %s\n",error.what());++failures;
    }
    std::printf("-- %d checks, %d failures --\n",checks,failures);
    return failures?1:0;
}
