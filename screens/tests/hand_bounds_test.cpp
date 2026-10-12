#include "../../native-portal/hand_bounds.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

int main() {
    std::mt19937 random(48326);
    std::uniform_real_distribution<float> position(-10,10), radius(.001f,.99f);
    std::array<float,512> packed{};
    for(unsigned count=0;count<=64;++count)
        for(int sample=0;sample<500;++sample) {
            for(unsigned i=0;i<count*2;++i) {
                for(int k=0;k<3;++k)packed[i*4+k]=position(random);
                packed[i*4+3]=radius(random);
            }
            float bounds[hand_bounds::kGroups*4];hand_bounds::compute(packed.data(),count,bounds);
            unsigned covered=0;
            for(unsigned group=0;group<hand_bounds::kGroups;++group) {
                unsigned begin,end;hand_bounds::range(count,group,begin,end);
                assert(end-begin<=8);
                assert(begin==covered);covered=end;
                const float *sphere=bounds+group*4;
                if(begin==end){for(int k=0;k<4;++k)assert(sphere[k]==0);continue;}
                for(unsigned i=begin;i<end;++i)
                    for(unsigned point=0;point<2;++point) {
                        const float *p=packed.data()+i*8+point*4;
                        double distance=0;
                        for(int k=0;k<3;++k){double delta=double(p[k])-sphere[k];distance+=delta*delta;}
                        // The whole endpoint sphere is inside the conservative bound.
                        // Its convex hull with the other endpoint sphere is inside too.
                        assert(std::sqrt(distance)+p[3]<=sphere[3]);
                    }
            }
            assert(covered==count);
        }
    std::puts("Hand bounds: all endpoint spheres contained");
}
