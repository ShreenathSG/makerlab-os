#include "core.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Independent exhaustive permutation oracle for small snapshots. */
static int exhaustive(const Job *jobs,size_t n,int work[3],unsigned done) {
    if(done==((1u<<n)-1))return 1;
    for(size_t i=0;i<n;i++) {
        if(done&(1u<<i))continue;
        int next[3],can=1;
        for(int r=0;r<3;r++){if(jobs[i].maximum[r]-jobs[i].allocation[r]>work[r])can=0;next[r]=work[r]+jobs[i].allocation[r];}
        if(can && exhaustive(jobs,n,next,done|(1u<<i)))return 1;
    }
    return 0;
}
static unsigned seed=42;
static unsigned random_value(void){seed=1664525u*seed+1013904223u;return seed;}
int main(void) {
    Job j[5]={0};size_t sequence[MAX_ACTIVE];int available[3]={0,0,0};
    j[0].maximum[0]=j[0].maximum[2]=j[1].maximum[0]=j[1].maximum[2]=1;
    j[0].allocation[0]=j[1].allocation[2]=1;
    assert(banker_safe(available,j,2,sequence)==0);
    j[1].allocation[2]=0;j[0].allocation[2]=1;
    assert(banker_safe(available,j,2,sequence)==1);assert(sequence[0]==0 && sequence[1]==1);
    available[0]=-1;assert(banker_safe(available,j,2,sequence)==-1);available[0]=0;
    j[0].allocation[0]=2;assert(banker_safe(available,j,2,sequence)==-1);
    assert(banker_safe(available,j,MAX_ACTIVE+1,sequence)==-1);
    assert(banker_safe(available,NULL,0,NULL)==1);
    for(int trial=0;trial<1000;trial++) {
        size_t n=random_value()%5+1;memset(j,0,sizeof(j));
        for(int r=0;r<3;r++)available[r]=(int)(random_value()%4);
        for(size_t i=0;i<n;i++)for(int r=0;r<3;r++){j[i].maximum[r]=(int)(random_value()%4);j[i].allocation[r]=(int)(random_value()%(unsigned)(j[i].maximum[r]+1));}
        int expected=exhaustive(j,n,available,0);assert(banker_safe(available,j,n,sequence)==expected);
        if(expected){int work[3];memcpy(work,available,sizeof(work));unsigned seen=0;
            for(size_t k=0;k<n;k++){size_t i=sequence[k];assert(i<n && !(seen&(1u<<i)));seen|=1u<<i;for(int r=0;r<3;r++){assert(j[i].maximum[r]-j[i].allocation[r]<=work[r]);work[r]+=j[i].allocation[r];}}
        }
    }
    memset(j,0,sizeof(j));
    j[0]=(Job){.id=1,.priority=1,.queued_at=0,.eligible=1};
    j[1]=(Job){.id=2,.priority=9,.queued_at=600,.eligible=1};
    j[2]=(Job){.id=3,.priority=10,.queued_at=0,.eligible=0};
    size_t count;priority_order(j,3,600,sequence,&count);assert(count==2 && sequence[0]==0);
    j[0].priority=9;j[0].queued_at=600;priority_order(j,3,600,sequence,&count);assert(sequence[0]==0);
    assert(effective_priority(&j[0],500)==9);assert(effective_priority(&j[0],1e20)==INT_MAX);
    puts("PASS: C safety cases, invalid input, aging, ties, and 1,000 exhaustive-oracle comparisons.");
    return 0;
}
