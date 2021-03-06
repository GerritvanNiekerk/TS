/* Copyright (C) 2012 Ion Torrent Systems, Inc. All Rights Reserved */


#include <string>
#include <vector>
#include <map>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <set>

#include "api/BamReader.h"
#include "api/SamHeader.h"
#include "api/BamAlignment.h"

#include "OptArgs.h"
#include "IonVersion.h"
#include <armadillo>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include "DPTreephaser.h"
#include "BaseCallerUtils.h"
#include "boost/multi_array.hpp"
#include "boost/shared_ptr.hpp"
#include "boost/make_shared.hpp"
#include "boost/algorithm/string.hpp"
#include "boost/lexical_cast.hpp"

#include <pthread.h>
#include <semaphore.h>

#include "FlowAlignment.h"
using namespace std;
using namespace BamTools;
using namespace arma;
using namespace boost;

const int MAX_NUM_THREADS = 24;
const int DEFAULT_NUM_THREADS = 8;
sem_t semLoadNull[MAX_NUM_THREADS];
sem_t semLoadFull[MAX_NUM_THREADS];
pthread_mutex_t mutexQuit;
pthread_mutex_t mutexNotDone;
bool quit[MAX_NUM_THREADS];
int notDone[MAX_NUM_THREADS];
BamAlignment bamAlignmentIn[MAX_NUM_THREADS];

typedef boost::multi_array<int, 3> int_3D_array;
typedef int_3D_array::index int_3D_index;

typedef boost::multi_array<int, 2> int_2D_array;
typedef int_2D_array::index int_2D_index;

typedef boost::multi_array<double, 2> double_2D_array;
typedef double_2D_array::index double_2D_index;

int PrintHelp()
{
  printf ("\n");
  printf ("Usage: calibrate [options]\n");
  printf ("\n");
  printf ("General options:\n");
  printf ("  -i,--input                 FILE       input mapped BAM [required option]\n");
  printf ("  --xMin                     INT        block minimum of X [0]\n");
  printf ("  --xMax                     INT        block maximum of X [1000000]\n");
  printf ("  --xCuts                    INT        paritions of X dimension [1]\n");
  printf ("  --yMin                     INT        block minimum of Y [0]\n");
  printf ("  --yMax                     INT        block maximum of Y  [1000000]\n");
  printf ("  --yCuts                    INT        paritions of Y dimension  [1]\n");
  printf ("  --numFlows                 INT        number of total flows  [10000]\n");
  printf ("  --flowCuts                 INT        flow parititions  [1]\n");
  printf ("  --numThreads               INT        number of threads running calibration function\n");
  printf ("  -o,--output-dir            FILE       output directory [current directory]\n");
  printf ("  --rawFile                  FILE       raw HP counting statistics [off]\n");
  printf ("  --hpModelFile              FILE       HP model file [{output-dir}/hpModel.txt]\n");
  printf ("  --hpTableStratified        FILE       stratified HP table [{output-dir}/flowQVtable.txt]\n");
  printf ("  --archiveFile              FILE       archive file of HP table [{output-dir}/HPTable.arc]\n");
  printf ("  --performMerge                        whether perform HPTable archivals merging [off]\n");
  printf ("  --mergeParentDir           FILE       parent directory of barcoded subfolders [off]\n");
  printf ("  --commonName               FILE       common archive file name of each barcoded dataset  [HPTable.arc]\n");
  printf ("  --hpTableAggregated        FILE       aggregated HP table [off]\n");
  printf ("  --alignmentAggregated      FILE       aggregated HP alignment summary [off]\n");
  printf ("  --alignmentStratified      FILE       stratified HP alignment [off]\n");
  printf ("  --skipDroop                           whether skip Droop [off]\n");
  printf ("\n");
  return 1;
}


struct Region{
    int xMin;
    int xMax;
    int yMin;
    int yMax;
    int flowStart;
    int flowEnd;
    char nuc;
    Region(int xMi, int xMa, int yMi, int yMa, int flowSt, int flowE, char n):
        xMin(xMi), xMax(xMa), yMin(yMi), yMax(yMa), flowStart(flowSt), flowEnd(flowE), nuc(n) {}
};

//static int partitionInd = 0;
struct HPPMDistribution{
  int numHPs;
  int recordsLimit;
  const static int MINIMUM_FIT_THRESHOLD = 50;
  vector<vector<double> > predictions;
  vector<vector<double> > measurements;
  vector<mat > models;
  HPPMDistribution(int hps, int rLimit):numHPs(hps), recordsLimit(rLimit){
      predictions.resize(numHPs, vector<double>());
      measurements.resize(numHPs, vector<double>());
      for(int hp=0; hp < numHPs; ++hp){
          predictions[hp].reserve(recordsLimit);
          measurements[hp].reserve(recordsLimit);
      }
  }

  //reserve memory for predictions and measurements for faster push_back
  void setRecordsLimit(int r){
      recordsLimit = r;
      for(int hp=0; hp < numHPs; ++hp){
          predictions[hp].reserve(recordsLimit);
          measurements[hp].reserve(recordsLimit);
      }
  }

  int getRecordsLimit(){
      return recordsLimit;
  }

  //should no-op be handled in higher level
  void add(int calledHP, int refHP, double prediction, double measurement){
    ////ignore dramtic shift observation; handle non-correct HP calls (effect?)
    if(prediction - measurement > 1 || measurement - prediction > 1)
      return;
    if(refHP >0 && refHP < numHPs && (int)predictions[refHP].size() < recordsLimit){
        predictions[refHP].push_back(prediction);
        measurements[refHP].push_back(measurement);
    }
  }

  void add(vector<vector<double> >& ps,  vector<vector<double> >& ms){

    for(int refHP=0; refHP < numHPs; ++refHP){
      int size = ps[refHP].size();
      for(int ind = 0; ind < size; ++ind){
        if((int)predictions[refHP].size() < recordsLimit - 1){
          predictions[refHP].push_back(ps[refHP][ind]);
          measurements[refHP].push_back(ms[refHP][ind]);
        }
      }
    }
  }

  void process(){
    mat defaultModel(2, 1);
    defaultModel(0, 0) = 1;
    defaultModel(1, 0) = 0;
//    //save prediction vs measurements
//    for (int hp = 1; hp <= 7; ++hp){
//        ostringstream stream;
//        stream << "Prediction_Measurement_P_" << partitionInd << "_HP_" << hp << ".txt";
//        FILE * pmFile = fopen (stream.str().c_str(),"w");
//        for(int ind = 0; ind < (int)predictions[hp].size(); ++ind){
//            fprintf(pmFile, "%6.4f\t%6.4f\n", predictions[hp][ind], measurements[hp][ind]);
//        }
//        fclose(pmFile);
//    }
//    partitionInd++;

    for(int hp = 0; hp < numHPs; ++hp){
        if((int)predictions[hp].size() > MINIMUM_FIT_THRESHOLD)
            models.push_back(fitFirstOrder(predictions[hp], measurements[hp]));
        else
          models.push_back(defaultModel);
    }
  }

  void print(FILE * hpTableFILE, Region r ){
    if(!hpTableFILE)
        return;
    for(int calledHP = 0; calledHP < numHPs; ++calledHP){
        fprintf(hpTableFILE, "%c\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%6.4f\t%6.4f\n", r.nuc, r.flowStart, r.flowEnd, r.xMin, r.xMax, r.yMin, r.yMax, calledHP, models[calledHP].at(0,0), models[calledHP].at(1,0));
    }
  }


};

struct AlignmentInfo{
  int refID;
  int position;
  AlignmentInfo(int r, int p):refID(r), position(p){
  }
  bool operator < (const AlignmentInfo & align) const{
      if(refID < align.refID){
        return true;
      } else if(refID > align.refID){
        return false;
      } else{
        if(position < align.position)
            return true;
        else
            return false;
      }
  }
  bool operator == (const AlignmentInfo & align) const{
      if(refID == align.refID && position == align.position)
          return true;
      else
          return false;
  }
};

struct HPPerturbationDistribution{
  int_3D_array refHPDistribution;
  double_2D_array delta;
//  int perturbationOffset;
  int numHPs;
  int numPerturbations;
  int_2D_array calibratedHP;
  double_2D_array accuracy;
  vector<double> maxAccuracy;
  vector<int> maxAccuracyIndices; //used to imputate and determine the center
  bool isProcessed;
  HPPerturbationDistribution(int hps, int perturbations):numHPs(hps),numPerturbations(perturbations){
      refHPDistribution.resize(boost::extents[numHPs][numPerturbations][numHPs]);
      isProcessed = false;
      //perturbationOffset=0; // make cppcheck happy
  }
  void add(int calledHP, int perturbation, int refHP){
      refHPDistribution[calledHP][perturbation][refHP]++;
      if(isProcessed) //reset it to false
        isProcessed = false;
  }

  void add(const int_3D_array& tempRefHPDistribution){
    //assume same size
    for(int calledHP=0; calledHP < numHPs; ++calledHP){
      for(int perturbation=0; perturbation < numPerturbations; ++perturbation){
        for(int refHP=0; refHP < numHPs; ++refHP){
          refHPDistribution[calledHP][perturbation][refHP] += tempRefHPDistribution[calledHP][perturbation][refHP];
        }
      }
    }
  }

  //aggregated one passed for protection
  void process(shared_ptr<HPPerturbationDistribution> parent){
      delta.resize(boost::extents[numHPs][numPerturbations]);
      calibratedHP.resize(boost::extents[numHPs][numPerturbations]);
      accuracy.resize(boost::extents[numHPs][numPerturbations]);
      maxAccuracyIndices.resize(numHPs);
      maxAccuracy.resize(numHPs);

      //identify the calibrated HP for each called HP at every perturbation; assign -1 if not available
      for(int hp = 0; hp < numHPs; ++hp){
        double tempMaxAccuracy = 0.0;
        int maxAccuracyIndex = -1;
        for(int perturb = 0; perturb < numPerturbations; ++perturb){
          int maxCount = 0;
          int totalCount = 0;
          int maxHP = -1;
          for(int refHP = 0; refHP < numHPs; ++refHP){
              totalCount += refHPDistribution[hp][perturb][refHP];
              if(refHPDistribution[hp][perturb][refHP] > maxCount){
                maxHP = refHP;
                maxCount = refHPDistribution[hp][perturb][refHP];
              }
          }
          if(maxCount > 50) { //adding robustness
              calibratedHP[hp][perturb] = maxHP;
              if(maxHP == hp){
                  accuracy[hp][perturb] = 1.0 * maxCount / totalCount;
                  if(tempMaxAccuracy <= accuracy[hp][perturb] && perturb > 0 && perturb < numPerturbations-1){
                      tempMaxAccuracy = accuracy[hp][perturb];
                      maxAccuracyIndex = perturb;
                  }
              }
          } else {
              calibratedHP[hp][perturb] = -1;
          }
        }

        if(tempMaxAccuracy > 0.5){
          maxAccuracyIndices[hp] = maxAccuracyIndex;
          maxAccuracy[hp] = tempMaxAccuracy;
        } else {
            maxAccuracyIndices[hp] = numPerturbations / 2; //center by default
            maxAccuracy[hp] = 0.33; //one third
        }

      }

      //imputation
      for(int hp = 0; hp < numHPs; ++hp){
        //protection in case complete missing
          if(calibratedHP[hp][maxAccuracyIndices[hp]] == -1){
              calibratedHP[hp][maxAccuracyIndices[hp]] = hp;
          }

          //left of maxAccuracy
          for(int perturb = maxAccuracyIndices[hp] - 1; perturb >= 0; perturb-- ){
              if(calibratedHP[hp][perturb] == -1){
                  if(parent )
                   calibratedHP[hp][perturb] = parent->calibratedHP[hp][perturb];
                  else
                    calibratedHP[hp][perturb] = calibratedHP[hp][perturb+1]; //carry from right
              }
          }

          //right of maxAccuracy
          for(int perturb = maxAccuracyIndices[hp] + 1; perturb < numPerturbations; perturb++){
              if(calibratedHP[hp][perturb] == -1){
                  if(parent )
                   calibratedHP[hp][perturb] = parent->calibratedHP[hp][perturb];
                  else
                    calibratedHP[hp][perturb] = calibratedHP[hp][perturb - 1]; //carry from left
              }
          }

      }

      //resolve violation
      for(int hp = 0; hp < numHPs; hp++){
          //left of maxAccuracy
          for(int perturb = maxAccuracyIndices[hp] - 1; perturb >= 0; perturb-- ){
              if(calibratedHP[hp][perturb] > calibratedHP[hp][perturb+1]){
                  calibratedHP[hp][perturb] = calibratedHP[hp][perturb+1]; //carry from right
              }
          }

          //right of maxAccuracy
          for(int perturb = maxAccuracyIndices[hp] + 1; perturb < numPerturbations; perturb++){
              if(calibratedHP[hp][perturb] < calibratedHP[hp][perturb-1]){
                  calibratedHP[hp][perturb] = calibratedHP[hp][perturb - 1]; //carry from left
              }
          }
      }

      vector<int> leftBoundaries(numHPs);
      vector<int> rightBoundaries(numHPs);
      for(int hp = 0; hp < numHPs; hp++){
          //default
          leftBoundaries[hp] = maxAccuracyIndices[hp] - 1;
          rightBoundaries[hp] = maxAccuracyIndices[hp] + 1;

          //left of maxAccuracy
          for(int perturb = maxAccuracyIndices[hp] - 1; perturb >= 0; perturb-- ){
              if(calibratedHP[hp][perturb] == calibratedHP[hp][perturb+1]){
                  leftBoundaries[hp] = perturb;
              } else {
                  break;
              }
          }

          //right of maxAccuracy
          for(int perturb = maxAccuracyIndices[hp] + 1; perturb < numPerturbations; perturb++){
              if(calibratedHP[hp][perturb] == calibratedHP[hp][perturb-1]){
                  rightBoundaries[hp] = perturb;
              } else {
                  break;
              }
          }
      }

      //connected
      for(int hp = 0; hp < numHPs - 1; hp++){
          if(rightBoundaries[hp] == numPerturbations - 1 && leftBoundaries[hp + 1] > 0){
              rightBoundaries[hp] = numPerturbations + leftBoundaries[hp + 1] - 1;
          }

          if(rightBoundaries[hp] < numPerturbations - 1 && leftBoundaries[hp + 1] == 0){
              leftBoundaries[hp + 1] = rightBoundaries[hp] + 1 - numPerturbations;
          }
      }

      //calculate the deltas for refHPs
      for(int hp = 0; hp < numHPs; hp++){
          for(int perturb = 0; perturb < numPerturbations; perturb++){
              int refHP = calibratedHP[hp][perturb];
              int refPerturb = perturb;
              if (refHP < hp){
                  refPerturb = numPerturbations + perturb;
              }
              if (refHP > hp){
                  refPerturb = perturb - numPerturbations;
              }
              delta[hp][perturb] = 1.0 * (numPerturbations - 1) * (refPerturb - leftBoundaries[refHP]) / (rightBoundaries[refHP] - leftBoundaries[refHP]) - refPerturb;
              //carry over from parent
              /*move carry over logic to low level
              if(parent && delta[hp][perturb] == 0 && parent->delta[hp][perturb] != 0){
                  delta[hp][perturb] = parent->delta[hp][perturb];
                  calibratedHP[hp][perturb] = parent->calibratedHP[hp][perturb];
              }
              */
          }
      }
      isProcessed = true;
  }

  void print(FILE * hpTableFILE, Region r ){
    if(!hpTableFILE)
        return;
    for(int calledHP = 0; calledHP < numHPs; ++calledHP){
        fprintf(hpTableFILE, "%c\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", r.nuc, r.flowStart, r.flowEnd, r.xMin, r.xMax, r.yMin, r.yMax, calledHP);
      for(int perturbation = 0; perturbation < numPerturbations;++perturbation){
          fprintf(hpTableFILE, "%d\t%d\t%6.2f\n", perturbation, calibratedHP[calledHP][perturbation], delta[calledHP][perturbation]);
      }
    }
  }

  void print(FILE * hpTableFILE, char nuc){
    if(!hpTableFILE)
        return;
    for(int calledHP = 0; calledHP < numHPs; ++calledHP){
        fprintf(hpTableFILE, "%c\t%d\n", nuc, calledHP);
      for(int perturbation = 0; perturbation < numPerturbations;++perturbation){
          fprintf(hpTableFILE, "%d\t%d\t%6.2f\n", perturbation, calibratedHP[calledHP][perturbation], delta[calledHP][perturbation]);
      }
    }
  }

};

struct HPTable{
  std::vector<Region> regionList;
  std::vector<boost::shared_ptr<HPPerturbationDistribution> > hpPerturbationDistributionList;
  std::vector<boost::shared_ptr<HPPerturbationDistribution> > aggregatedHPPerturbationDistribution;
  std::vector<boost::shared_ptr<HPPMDistribution> > hpPMDistributionList;
  std::vector<boost::shared_ptr<set<AlignmentInfo> > > alignmentStratifiedList; //size: numRegions * numHPs
  std::vector<boost::shared_ptr<set<AlignmentInfo> > > alignmentAggregatedList; //size: numNucs * numHPs

  int xOverallMin;
  int xOverallMax;
  int xOverallCuts;
  int xSpan;
  int yOverallMin;
  int yOverallMax;
  int yOverallCuts;
  int ySpan;
  int totalFlows;
  int flowOverallCuts;
  int flowSpan;
  int numRegions;
  int numPerturbations; //0-99
  int numHPs; // upto hp of 11, how about hp of 0
  bool isProcessed;
  int recordsLimit;
  const static int numNucs = 4; //A,C, G, T
  vector<char> nucs;
  const static unsigned int ALIGN_RECORD_LIMIT = 1000; //parameter to fine tune

  static int NucToInt (char nuc) {
      if (nuc=='a' or nuc=='A') return 0;
      if (nuc=='c' or nuc=='C') return 1;
      if (nuc=='g' or nuc=='G') return 2;
      if (nuc=='t' or nuc=='T') return 3;
      return -1;
  }

  static char IntToNuc(int index){
      switch(index){
        case 0:
          return 'A';
        case 1:
          return 'C';
        case 2:
          return 'G';
        case 3:
          return 'T';
      }
      return ' ';
  }

  int calcRegionIndex(char nuc, int flow, int x, int y){
      return NucToInt(nuc) + numNucs * (flow/flowSpan + ((y - yOverallMin)/ySpan + (x-xOverallMin)/xSpan*yOverallCuts) * flowOverallCuts );
  }

  HPTable(int xMin, int xMax, int xCuts, int yMin, int yMax, int yCuts, int numFlows, int flowCuts, int perturbations, int hps, int rLimit=10000):
      xOverallMin(xMin), xOverallMax(xMax), xOverallCuts(xCuts), yOverallMin(yMin), yOverallMax(yMax), yOverallCuts(yCuts), totalFlows(numFlows), flowOverallCuts(flowCuts), numPerturbations(perturbations), numHPs(hps),recordsLimit(rLimit)
  {
    //calculate xSpan, ySpan and flowSpan
    xSpan = (xMax - xMin + 2) / xCuts;
    ySpan = (yMax - yMin + 2) / yCuts;
    flowSpan = numFlows / flowCuts;
    char nucList[4] = {'A', 'C', 'G', 'T'};
    numRegions = xCuts * yCuts * flowCuts * numNucs;
    regionList.reserve(numRegions);
    for(int xCut = 0; xCut < xCuts; ++xCut){
        for(int yCut = 0; yCut < yCuts; ++yCut){
            for(int flowCut = 0; flowCut < flowCuts; ++flowCut){
                for(int nucInd = 0; nucInd < numNucs; ++nucInd){
                    regionList.push_back(Region(xOverallMin + xCut*xSpan, xOverallMin + (1+xCut) * xSpan - 1,
                                                yOverallMin + yCut*ySpan, yOverallMin + (1+yCut) * ySpan - 1,
                                                flowCut*flowSpan, (flowCut + 1)*flowSpan - 1,
                                                nucList[nucInd])
                                         );
                }
            }
        }
    }

    //create hpPerturbationDistributionList
    hpPerturbationDistributionList.reserve(numRegions);
    for(int regionInd = 0; regionInd < numRegions; ++regionInd){
        hpPerturbationDistributionList.push_back(make_shared<HPPerturbationDistribution>(HPPerturbationDistribution(numHPs, numPerturbations)));
    }

    //create hpPMDistributionList
    hpPMDistributionList.reserve(numRegions);
    for(int regionInd = 0; regionInd < numRegions; ++regionInd){
        hpPMDistributionList.push_back(make_shared<HPPMDistribution>(HPPMDistribution(numHPs, recordsLimit)));
    }

    //create alignmentStratifiedList startified*numHPs
    for(int alignInd = 0; alignInd < numRegions*numHPs; ++alignInd){
        alignmentStratifiedList.push_back(make_shared<set<AlignmentInfo> >());
    }

    //create aggregatedHPPerturbationDistribution
    aggregatedHPPerturbationDistribution.reserve(numNucs);
    for(int nucInd = 0; nucInd < numNucs; ++nucInd){
        aggregatedHPPerturbationDistribution.push_back(make_shared<HPPerturbationDistribution>(HPPerturbationDistribution(numHPs, numPerturbations)));
    }

    //create alignmentAggregatedList aggregated*numHPs
    for(int alignInd = 0; alignInd < numNucs*numHPs; ++alignInd){
        alignmentAggregatedList.push_back(make_shared<set<AlignmentInfo> >());
    }

    isProcessed = false;

  }

  void addHPTable(HPTable* hpTablePtr){
    if(hpTablePtr == 0)
      return;
    isProcessed = false;
    //add records to   std::vector<boost::shared_ptr<HPPerturbationDistribution> > hpPerturbationDistributionList;
    for(int ind = 0; ind < (int)hpPerturbationDistributionList.size(); ++ind){
        hpPerturbationDistributionList[ind]->add(hpTablePtr->hpPerturbationDistributionList[ind]->refHPDistribution);
    }

    //add records to   std::vector<boost::shared_ptr<HPPerturbationDistribution> > aggregatedHPPerturbationDistribution;
    for(int ind = 0; ind < (int)aggregatedHPPerturbationDistribution.size(); ++ind){
        aggregatedHPPerturbationDistribution[ind]->add(hpTablePtr->aggregatedHPPerturbationDistribution[ind]->refHPDistribution);
    }

    //add records to   std::vector<boost::shared_ptr<HPPMDistribution> > hpPMDistributionList;
    for(int ind = 0; ind < (int)hpPMDistributionList.size(); ++ind){
        hpPMDistributionList[ind]->add(hpTablePtr->hpPMDistributionList[ind]->predictions, hpTablePtr->hpPMDistributionList[ind]->measurements);
    }


    for(int ind = 0; ind < (int)alignmentStratifiedList.size(); ++ind){
      set<AlignmentInfo>::iterator it;
      set<AlignmentInfo> *alignmentStratified =  (hpTablePtr->alignmentStratifiedList[ind]).get();
      for(it = alignmentStratified->begin(); it != alignmentStratified->end(); ++it){
          if(alignmentStratifiedList[ind]->size() < ALIGN_RECORD_LIMIT){
            //add it if not in the set
            if(alignmentStratifiedList[ind] -> find(*it) == alignmentStratifiedList[ind]->end()){
                alignmentStratifiedList[ind]-> insert(*it);
            }
          } else {
            goto end_stratified_loop;
          }
      }
    }
    end_stratified_loop:

    for(int ind = 0; ind < (int)alignmentAggregatedList.size(); ++ind){
      set<AlignmentInfo>::iterator it;
      set<AlignmentInfo> *alignmentAggregated =  hpTablePtr->alignmentAggregatedList[ind].get();
      for(it = alignmentAggregated->begin(); it != alignmentAggregated->end(); ++it){
          if(alignmentAggregatedList[ind]->size() < ALIGN_RECORD_LIMIT){
            //add it if not in the set
            if(alignmentAggregatedList[ind] -> find(*it) == alignmentAggregatedList[ind]->end()){
                alignmentAggregatedList[ind]-> insert(*it);
            }
          } else {
            goto end_aggregated_loop;
          }
      }
    }
    end_aggregated_loop:
    return;

    //add records to   std::vector<boost::shared_ptr<boost::unordered_set<AlignmentInfo> > > alignmentStratifiedList; //size: numRegions * numHPs
    //add records to   std::vector<boost::shared_ptr<boost::unordered_set<AlignmentInfo> > > alignmentAggregatedList; //size: numNucs * numHPs
  }

  void addAlignmentRecord(int calledHP, int refHP, int perturbation, double prediction, double measurement, AlignmentInfo alignInfo, char nuc, int flow, int x, int y){
    //get region index
    int regionIndex = calcRegionIndex(nuc, flow, x, y);
    //boundary check

    if(calledHP >= numHPs || refHP >= numHPs || perturbation >= numPerturbations || regionIndex >= numRegions || regionIndex < 0)
      return;
    aggregatedHPPerturbationDistribution[NucToInt(nuc)]->add(calledHP, perturbation, refHP);
    hpPerturbationDistributionList[regionIndex]->add(calledHP, perturbation, refHP);
    //refHP
    hpPMDistributionList[regionIndex]->add(calledHP, refHP, prediction, measurement);
    //nuc-based
//    hpPMDistributionList[NucToInt(nuc)]->add(calledHP, refHP, prediction, measurement);

    int alignStratifiedIndex = regionIndex*numHPs + refHP;
    if(alignmentStratifiedList[alignStratifiedIndex]->size() < ALIGN_RECORD_LIMIT){
      //add it if not in the set
        if(alignmentStratifiedList[alignStratifiedIndex] -> find(alignInfo) == alignmentStratifiedList[alignStratifiedIndex]->end()){
            alignmentStratifiedList[alignStratifiedIndex]-> insert(alignInfo);
        }
    }

    int alignAggregatedIndex = NucToInt(nuc)*numHPs + refHP;
    if(alignmentAggregatedList[alignAggregatedIndex]->size() < ALIGN_RECORD_LIMIT){
      //add it if not in the set
        if(alignmentAggregatedList[alignAggregatedIndex] -> find(alignInfo) == alignmentAggregatedList[alignAggregatedIndex]->end()){
            alignmentAggregatedList[alignAggregatedIndex]-> insert(alignInfo);
        }
    }
  }

  void addAlignmentRecord(int calledHP, int refHP, int perturbation, AlignmentInfo alignInfo, char nuc, int flow, int x, int y){
    //get region index
    int regionIndex = calcRegionIndex(nuc, flow, x, y);
    //boundary check

    if(calledHP >= numHPs || refHP >= numHPs || perturbation >= numPerturbations || regionIndex >= numRegions || regionIndex < 0)
      return;
    aggregatedHPPerturbationDistribution[NucToInt(nuc)]->add(calledHP, perturbation, refHP);
    hpPerturbationDistributionList[regionIndex]->add(calledHP, perturbation, refHP);

    int alignStratifiedIndex = regionIndex*numHPs + refHP;
    if(alignmentStratifiedList[alignStratifiedIndex]->size() < ALIGN_RECORD_LIMIT){
      //add it if not in the set
        if(alignmentStratifiedList[alignStratifiedIndex] -> find(alignInfo) == alignmentStratifiedList[alignStratifiedIndex]->end()){
            alignmentStratifiedList[alignStratifiedIndex]-> insert(alignInfo);
        }
    }
    int alignAggregatedIndex = NucToInt(nuc)*numHPs + refHP;
    if(alignmentAggregatedList[alignAggregatedIndex]->size() < ALIGN_RECORD_LIMIT){
      //add it if not in the set
        if(alignmentAggregatedList[alignAggregatedIndex] -> find(alignInfo) == alignmentAggregatedList[alignAggregatedIndex]->end()){
            alignmentAggregatedList[alignAggregatedIndex]-> insert(alignInfo);
        }
    }
  }

  void process(){
    //process aggregated table
    for(int nucInd = 0; nucInd < numNucs; ++nucInd){
        aggregatedHPPerturbationDistribution[nucInd]->process(shared_ptr<HPPerturbationDistribution>()); //null shared_ptr
    }

    //stratification handling
    for(int regionInd = 0; regionInd < numRegions; ++regionInd){
        hpPerturbationDistributionList[regionInd] -> process(aggregatedHPPerturbationDistribution[NucToInt( regionList[regionInd].nuc)]);
        hpPMDistributionList[regionInd]->process();
    }

    //low complexity handling

    isProcessed = true;
  }

  void saveToFile(const char* fileName){
    ofstream file (fileName, ios::binary);
    if (file.is_open())
    {
      //save primitive types
      file.write(reinterpret_cast<const char *>(&xOverallMin), 4);
      file.write(reinterpret_cast<const char *>(&xOverallMax), 4);
      file.write(reinterpret_cast<const char *>(&xOverallCuts), 4);
      file.write(reinterpret_cast<const char *>(&yOverallMin), 4);
      file.write(reinterpret_cast<const char *>(&yOverallMax), 4);
      file.write(reinterpret_cast<const char *>(&yOverallCuts), 4);
      file.write(reinterpret_cast<const char *>(&totalFlows), 4);
      file.write(reinterpret_cast<const char *>(&flowOverallCuts), 4);
      file.write(reinterpret_cast<const char *>(&numPerturbations), 4);
      file.write(reinterpret_cast<const char *>(&numHPs), 4);
      file.write(reinterpret_cast<const char *>(&recordsLimit), 4);
      file.flush();

      //save hpPerturbationDistributionList
      for(int regionInd = 0; regionInd < numRegions; ++regionInd){
          int_3D_array& refHPDist =  hpPerturbationDistributionList[regionInd]->refHPDistribution;
          for(int calledHP = 0; calledHP < numHPs; ++calledHP){
              for(int perturb = 0; perturb < numPerturbations; ++perturb){
                  for(int refHP = 0; refHP < numHPs; ++refHP){
                      file.write(reinterpret_cast<const char *>(&refHPDist[calledHP][perturb][refHP]), 4);
                  }
              }
          }
      }
      file.flush();

      //save aggregatedHPPerturbationDistribution
      for(int nucInd = 0; nucInd < numNucs; ++nucInd){
          int_3D_array& refHPDist =  aggregatedHPPerturbationDistribution[nucInd]->refHPDistribution;
          for(int calledHP = 0; calledHP < numHPs; ++calledHP){
              for(int perturb = 0; perturb < numPerturbations; ++perturb){
                  for(int refHP = 0; refHP < numHPs; ++refHP){
                      file.write(reinterpret_cast<const char *>(&refHPDist[calledHP][perturb][refHP]), 4);
                  }
              }
          }
      }
      file.flush();

      //save hpPMDistributionList
      for(int regionInd = 0; regionInd < numRegions; ++regionInd){
          HPPMDistribution* hpPMDist =  hpPMDistributionList[regionInd].get();
          for(int refHP = 0; refHP < numHPs; ++refHP){
              int records = hpPMDist->predictions[refHP].size();
              file.write(reinterpret_cast<const char *>(&records), 4);
              if(records != 0)
              for(int ind = 0; ind < records; ++ind){
                  file.write(reinterpret_cast<const char *>(&(hpPMDist->predictions[refHP][ind])), sizeof(double));
                file.write(reinterpret_cast<const char *>(&(hpPMDist->measurements[refHP][ind])), sizeof(double));
              }
          }
      }
      file.flush();


      //save stratified alignmentInfo
      for(int ind = 0; ind < (int)alignmentStratifiedList.size(); ++ind){
        int setSize = alignmentStratifiedList[ind]->size();
        file.write(reinterpret_cast<const char *>(&setSize), 4);
        if(setSize > 0){
            set<AlignmentInfo>::iterator it;
            set<AlignmentInfo> *alignmentStratified =  alignmentStratifiedList[ind].get();
            for(it = alignmentStratified->begin(); it != alignmentStratified->end(); ++it){
                file.write(reinterpret_cast<const char *>(&(it->refID)), 4);
                file.write(reinterpret_cast<const char *>(&(it->position)), 4);
            }
        }
      }
      file.flush();

      //save aggregated alignmentInfo
      for(int ind = 0; ind < (int)alignmentAggregatedList.size(); ++ind){
        int setSize = alignmentAggregatedList[ind]->size();
        file.write(reinterpret_cast<const char *>(&setSize), 4);
        if(setSize > 0){
            set<AlignmentInfo>::iterator it;
            set<AlignmentInfo> *alignmentStratified =  alignmentAggregatedList[ind].get();
            for(it = alignmentStratified->begin(); it != alignmentStratified->end(); ++it){
                file.write(reinterpret_cast<const char *>(&(it->refID)), 4);
                file.write(reinterpret_cast<const char *>(&(it->position)), 4);
            }
        }
      }
      file.flush();


      file.close();
    }
    else
      printf("Unable to open file\n");
    //add checksum
  }

  void printStratifiedAlignSummary(const char* fileName){
    FILE * alignFile = fopen (fileName,"w");
    if(alignFile == 0){
        printf("%s does not exist", fileName);
        return;
    }
    for(int regionInd = 0; regionInd < numRegions; ++regionInd){
        Region r = regionList[regionInd];
        for(int hp = 0; hp < numHPs; ++hp){
          fprintf(alignFile, "%c\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", r.nuc, r.flowStart, r.flowEnd, r.xMin, r.xMax, r.yMin, r.yMax, hp, (int)alignmentStratifiedList[regionInd*numHPs + hp]->size());
        }
    }
    fclose(alignFile);
  }

  void printAggregatedAlignSummary(const char* fileName){
      FILE * alignFile = fopen (fileName,"w");
      if(alignFile == 0){
          printf("%s does not exist", fileName);
          return;
      }
      for(int nucInd = 0; nucInd < numNucs; ++nucInd){
          for(int hp = 0; hp < numHPs; ++hp){
            fprintf(alignFile, "%c\t%d\t%d\n", IntToNuc(nucInd), hp, (int)alignmentStratifiedList[nucInd*numHPs + hp]->size());
          }
      }
      fclose(alignFile);
  }

  //create calibration table, correctedHPs, accuracy
  void printRaw(const char* fileName){
    //save hptable to verify the correctness
    FILE * hpTableFile = fopen (fileName,"w");
    if(hpTableFile == 0){
        printf("%s does not exist", fileName);
        return;
    }
    fprintf(hpTableFile, "aggregated table\n");
    for(int nucInd = 0; nucInd < numNucs; ++nucInd){
        for(int calledHP = 0; calledHP < numHPs; ++calledHP){
            fprintf(hpTableFile, "%c\tCalledHP: %d\n", IntToNuc(nucInd), calledHP);
            for(int perturbation = 0; perturbation < numPerturbations;++perturbation){
              fprintf(hpTableFile, "%d", perturbation);
              for(int refHP = 0; refHP < numHPs; ++refHP){
                  fprintf(hpTableFile, ",%d", aggregatedHPPerturbationDistribution[nucInd]->refHPDistribution[calledHP][perturbation][refHP]);
              }
              fprintf(hpTableFile, "\n");
            }
        }
    }
    fprintf(hpTableFile, "stratified table\n");
    for(int regionInd = 0; regionInd < numRegions; ++regionInd){
        Region r = regionList[regionInd];
        for(int calledHP = 0; calledHP < numHPs; ++calledHP){
            fprintf(hpTableFile, "%c\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", r.nuc, r.flowStart, r.flowEnd, r.xMin, r.xMax, r.yMin, r.yMax, calledHP);
            for(int perturbation = 0; perturbation < numPerturbations;++perturbation){
              fprintf(hpTableFile, "%d", perturbation);
              for(int refHP = 0; refHP < numHPs; ++refHP){
                  fprintf(hpTableFile, ",%d", hpPerturbationDistributionList[regionInd]->refHPDistribution[calledHP][perturbation][refHP]);
              }
              fprintf(hpTableFile, "\n");
            }
        }
    }
    fclose(hpTableFile);
  }

  void printStratifiedTable(const char* fileName){
    FILE * hpTableFile = fopen (fileName,"w");
    if(hpTableFile == 0){
        printf("%s does not exist", fileName);
        return;
    }
    fprintf(hpTableFile, "#flowStart\tflowEnd\tflowSpan\txMin\txMax\txSpan\tyMin\tyMax\tySpan\tmaxHPLength\n");
    fprintf(hpTableFile, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", 0, totalFlows-1, flowSpan, xOverallMin, xOverallMax, xSpan, yOverallMin, yOverallMax, ySpan, numHPs);
    for(int regionInd = 0; regionInd < numRegions; ++regionInd){
        hpPerturbationDistributionList[regionInd] -> print(hpTableFile, regionList[regionInd]);
    }
    fclose(hpTableFile);
  }

  void printStratifiedModels(const char* fileName){
    FILE * hpTableFile = fopen (fileName,"w");
    if(hpTableFile == 0){
        printf("%s does not exist", fileName);
        return;
    }
    //print header
    fprintf(hpTableFile, "#flowStart\tflowEnd\tflowSpan\txMin\txMax\txSpan\tyMin\tyMax\tySpan\tmaxHPLength\n");
    fprintf(hpTableFile, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", 0, totalFlows-1, flowSpan, xOverallMin, xOverallMax, xSpan, yOverallMin, yOverallMax, ySpan, numHPs);
    for(int regionInd = 0; regionInd < numRegions; ++regionInd){
        hpPMDistributionList[regionInd] -> print(hpTableFile, regionList[regionInd]);
    }
    fclose(hpTableFile);
  }

  void printAggregatedTable(const char* fileName){
    FILE * hpTableFile = fopen (fileName,"w");
    if(hpTableFile == 0){
        printf("%s does not exist", fileName);
        return;
    }
    for(int nucInd = 0; nucInd < numNucs; ++nucInd){
        aggregatedHPPerturbationDistribution[nucInd] -> print(hpTableFile, IntToNuc(nucInd));
    }
    fclose(hpTableFile);
  }
};

HPTable* mergeHPTables(vector<shared_ptr<HPTable> > hpTableList){
    if(hpTableList.size() == 0)
        return 0;
    //the list of HPTable has the same dimensions for now
    HPTable* hp = hpTableList[0].get();
    int numHPTables = hpTableList.size();
    HPTable* mergedTable = new HPTable(hp->xOverallMin, hp->xOverallMax, hp->xOverallCuts, hp->yOverallMin, hp->yOverallMax, hp->yOverallCuts, hp->totalFlows, hp->flowOverallCuts, hp->numPerturbations, hp->numHPs, numHPTables * hp->recordsLimit);
    //start merging efforts
    for(int ind = 0; ind < numHPTables; ++ind){
        mergedTable->addHPTable(hpTableList[ind].get());
    }
    return mergedTable;
}

HPTable* loadHPTable(const char* hpTableFile){
    ifstream file (hpTableFile, ios::binary);
    HPTable* loadedTable = 0;
    if (file.is_open())
    {
      //save primitive types
      int xOverallMin;
      file.read(reinterpret_cast<char *>(&xOverallMin), 4);
      int xOverallMax;
      file.read(reinterpret_cast<char *>(&xOverallMax), 4);
      int xOverallCuts;
      file.read(reinterpret_cast<char *>(&xOverallCuts), 4);
      int yOverallMin;
      file.read(reinterpret_cast<char *>(&yOverallMin), 4);
      int yOverallMax;
      file.read(reinterpret_cast<char *>(&yOverallMax), 4);
      int yOverallCuts;
      file.read(reinterpret_cast<char *>(&yOverallCuts), 4);
      int totalFlows;
      file.read(reinterpret_cast<char *>(&totalFlows), 4);
      int flowOverallCuts;
      file.read(reinterpret_cast<char *>(&flowOverallCuts), 4);
      int numPerturbations;
      file.read(reinterpret_cast<char *>(&numPerturbations), 4);
      int numHPs;
      file.read(reinterpret_cast<char *>(&numHPs), 4);
      int recordsLimit;
      file.read(reinterpret_cast<char *>(&recordsLimit), 4);
      loadedTable = new HPTable(xOverallMin, xOverallMax, xOverallCuts, yOverallMin, yOverallMax, yOverallCuts, totalFlows, flowOverallCuts, numPerturbations, numHPs,recordsLimit);
      int numRegions = loadedTable->numRegions;
      //save hpPerturbationDistributionList
      for(int regionInd = 0; regionInd < numRegions; ++regionInd){
          int_3D_array& refHPDist =  loadedTable->hpPerturbationDistributionList[regionInd]->refHPDistribution;
          for(int calledHP = 0; calledHP < numHPs; ++calledHP){
              for(int perturb = 0; perturb < numPerturbations; ++perturb){
                  for(int refHP = 0; refHP < numHPs; ++refHP){
                      file.read(reinterpret_cast<char *>(&refHPDist[calledHP][perturb][refHP]), 4);
                  }
              }
          }
      }

      //load aggregatedHPPerturbationDistribution
      int numNucs = loadedTable->numNucs;
      for(int nucInd = 0; nucInd < numNucs; ++nucInd){
          int_3D_array& refHPDist =  loadedTable->aggregatedHPPerturbationDistribution[nucInd]->refHPDistribution;
          for(int calledHP = 0; calledHP < numHPs; ++calledHP){
              for(int perturb = 0; perturb < numPerturbations; ++perturb){
                  for(int refHP = 0; refHP < numHPs; ++refHP){
                      file.read(reinterpret_cast<char *>(&refHPDist[calledHP][perturb][refHP]), 4);
                  }
              }
          }
      }

      //load hpPMDistributionList
      for(int regionInd = 0; regionInd < numRegions; ++regionInd){
          HPPMDistribution* hpPMDist =  loadedTable->hpPMDistributionList[regionInd].get();
          for(int refHP = 0; refHP < numHPs; ++refHP){
              int records = hpPMDist->predictions[refHP].size();
              file.read(reinterpret_cast<char *>(&records), 4);
              if(records > 0){
                  for(int ind = 0; ind < records; ++ind){
                    double prediction;
                    file.read(reinterpret_cast<char *>(&prediction), sizeof(double));
                    hpPMDist->predictions[refHP].push_back(prediction);
                    double measurement;
                    file.read(reinterpret_cast<char *>(&measurement), sizeof(double));
                    hpPMDist->measurements[refHP].push_back(measurement);
                  }
              }
          }
      }

      //load alignStratified
      for(int ind = 0; ind < (int)loadedTable->alignmentStratifiedList.size(); ++ind){
        int setSize = loadedTable->alignmentStratifiedList[ind]->size();
        file.read(reinterpret_cast<char *>(&setSize), 4);
        while(setSize > 0){
            int refID = 0;
            int position = 0;
            file.read(reinterpret_cast<char *>(&refID), 4);
            file.read(reinterpret_cast<char *>(&position), 4);
            loadedTable->alignmentStratifiedList[ind]->insert(AlignmentInfo(refID, position));
            setSize --;
        }
      }

      //load alignStratified
      for(int ind = 0; ind < (int)loadedTable->alignmentAggregatedList.size(); ++ind){
        int setSize = loadedTable->alignmentAggregatedList[ind]->size();
        file.read(reinterpret_cast<char *>(&setSize), 4);
        while(setSize > 0){
            int refID = 0;
            int position = 0;
            file.read(reinterpret_cast<char *>(&refID), 4);
            file.read(reinterpret_cast<char *>(&position), 4);
            loadedTable->alignmentAggregatedList[ind]->insert(AlignmentInfo(refID, position));
            setSize --;
        }
      }

      file.close();
      return loadedTable;
    }
    else{
//      printf("Unable to open file\n");
      return 0;
    }
}

typedef struct loadArg
{
	int numThreads;
	BamReader* bamReader;
}loadArg;

void* LoadFunc(void* arg0)
{
    loadArg* arg = (loadArg*)arg0;
	int threadIndex = 0;
	string read_group;
	if(arg)
	{
		while(arg->bamReader)
		{				
			sem_wait(&semLoadNull[threadIndex]);

			bamAlignmentIn[threadIndex].QueryBases.clear();
			bamAlignmentIn[threadIndex].Qualities.clear();
			bamAlignmentIn[threadIndex].RemoveTag("RG");
			bamAlignmentIn[threadIndex].RemoveTag("PG");
			bamAlignmentIn[threadIndex].RemoveTag("ZF"); 	
			bamAlignmentIn[threadIndex].RemoveTag("FZ");	
			bamAlignmentIn[threadIndex].RemoveTag("ZM");
			bamAlignmentIn[threadIndex].RemoveTag("ZP");
			bamAlignmentIn[threadIndex].RemoveTag("MD");

			if((arg->bamReader)->GetNextAlignment(bamAlignmentIn[threadIndex]))
			{
				if(!bamAlignmentIn[threadIndex].IsMapped() or !bamAlignmentIn[threadIndex].GetTag("RG",read_group) or
					!bamAlignmentIn[threadIndex].HasTag("MD") or !bamAlignmentIn[threadIndex].HasTag("ZF") or 
					!bamAlignmentIn[threadIndex].HasTag("FZ") or !bamAlignmentIn[threadIndex].HasTag("ZM"))
				{
					sem_post(&semLoadNull[threadIndex]);
				}
				else
				{
					pthread_mutex_lock(&mutexNotDone);
					notDone[threadIndex]++;
					pthread_mutex_unlock(&mutexNotDone);
					sem_post(&semLoadFull[threadIndex]);					
					threadIndex = (threadIndex + 1) % arg->numThreads;
				}
			}
			else
			{					
				break;
			}
		}		
	}

    for(int i = 0; i < arg->numThreads; ++i)
    {
		pthread_mutex_lock(&mutexQuit);
        quit[threadIndex] = true;
		pthread_mutex_unlock(&mutexQuit);
        sem_post(&semLoadFull[threadIndex]);
		threadIndex = (threadIndex + 1) % arg->numThreads;
    }

	cout << "Loading thread exits." << endl;

	return NULL;
}

typedef struct recallArg
{
	int threadIndex;
	int* numAlignedReads;
	HPTable* hptable;
    bool skipDroop;
	map<string,string>  key_by_read_group;
	map<string,string> flow_order_by_read_group;
}recallArg;

void* RecallFunc(void* arg0)
{
    recallArg* arg = (recallArg*)arg0;
	if(arg)
	{
		bool myQuit = false;
        sem_wait(&semLoadFull[arg->threadIndex]);
		pthread_mutex_lock(&mutexQuit);
		myQuit = quit[arg->threadIndex];
		pthread_mutex_unlock(&mutexQuit);
		if(myQuit)
		{
			pthread_mutex_lock(&mutexNotDone);
			if(notDone[arg->threadIndex] > 0)
			{
				myQuit = false;
			}
			pthread_mutex_unlock(&mutexNotDone);
		}
		while(!myQuit)
        {
			//
			// Step 1: Retrieve misc tags
			//
			pthread_mutex_lock(&mutexNotDone);
			notDone[arg->threadIndex]--;
			pthread_mutex_unlock(&mutexNotDone);

			string read_group;
            bamAlignmentIn[arg->threadIndex].GetTag("RG",read_group);

			const string&     main_flow_order = arg->flow_order_by_read_group[read_group];
			int               first_useful_flow;
			vector<uint16_t>  fz_tag;
			string            md_tag;
			vector<int16_t>   zm_tag; //convert to real zm values by dividing it by 256.0, i.e., multiplying it by 0.00390625
			vector<float>     zp_tag;

			bamAlignmentIn[arg->threadIndex].GetTag("ZF", first_useful_flow);
			bamAlignmentIn[arg->threadIndex].GetTag("FZ", fz_tag);
			bamAlignmentIn[arg->threadIndex].GetTag("MD", md_tag);
			bamAlignmentIn[arg->threadIndex].GetTag("ZM", zm_tag);
			bamAlignmentIn[arg->threadIndex].GetTag("ZP", zp_tag);
			//if(zp_tag.size() != 3)
			//	throw std::runtime_error("ZP must have three values");

			std::vector<std::string> strs;
			boost::split(strs, bamAlignmentIn[arg->threadIndex].Name, boost::is_any_of(":"));
            int y = boost::lexical_cast<int>(strs[1]);
            int x = boost::lexical_cast<int>(strs[2]);
			//get tag corresponding to measurements

            int refID = bamAlignmentIn[arg->threadIndex].RefID;
            int position = bamAlignmentIn[arg->threadIndex].Position;
            bool isReversedStrand = bamAlignmentIn[arg->threadIndex].IsReverseStrand();

			//
			// Step 2: Parse and retrieve the base alignments and reference sequence
			//

			string tseq_bases;
			string qseq_bases;
			string pretty_tseq;
			string pretty_qseq;
			string pretty_aln;

			RetrieveBaseAlignment(bamAlignmentIn[arg->threadIndex].QueryBases, bamAlignmentIn[arg->threadIndex].CigarData, md_tag,
				tseq_bases, qseq_bases, pretty_tseq, pretty_qseq, pretty_aln);

		//    printf("Base Q prior to reverse: %s\n", qseq_bases.c_str());
			if (bamAlignmentIn[arg->threadIndex].IsReverseStrand()) {
			  ReverseComplementInPlace(qseq_bases);
			  ReverseComplementInPlace(tseq_bases);
			  ReverseComplementInPlace(pretty_qseq);
			  ReverseComplementInPlace(pretty_aln);
			  ReverseComplementInPlace(pretty_tseq);
			}
		//    printf("Base Q after reverse: %s\n", qseq_bases.c_str());

			/*
			for (int offset = 0; offset < (int)pretty_qseq.size(); offset += 150) {
			  printf("Base Q% 4d: %s\n",    offset, pretty_qseq.substr(offset,150).c_str());
			  printf("Base A% 4d: %s\n",    offset, pretty_aln.substr(offset,150).c_str());
			  printf("Base T% 4d: %s\n\n",  offset, pretty_tseq.substr(offset,150).c_str());
			}
			*/

			sem_post(&semLoadNull[arg->threadIndex]);

			//
			// Step 3: Execute flow realignment procedure
			//

			vector<char>    flowOrder;    // The flow order for the alignment, including deleted reference bases.
			vector<int>     qseq;         // The query or read sequence in the alignment, including gaps.
			vector<int>     tseq;         // The target or reference sequence in the alignment, including gaps.
			vector<int>     perturbation; // The perturbation. TODO: verify whether including gaps
			vector<char>    aln;          // The alignment string

			if (!PerformFlowAlignment(tseq_bases, qseq_bases, main_flow_order, first_useful_flow, fz_tag,
				flowOrder, qseq, tseq, perturbation, aln))
			{
				sem_wait(&semLoadFull[arg->threadIndex]);			
				pthread_mutex_lock(&mutexQuit);
				myQuit = quit[arg->threadIndex];
				pthread_mutex_unlock(&mutexQuit);
				if(myQuit)
				{
					pthread_mutex_lock(&mutexNotDone);
					if(notDone[arg->threadIndex] > 0)
					{
						myQuit = false;
					}
					pthread_mutex_unlock(&mutexNotDone);
				}
				continue;
			}

			int nonEmptyFlowFirst = 0; // The index of the first non-empty read flow
			for(int q_idx = 0; q_idx < (int)qseq.size(); ++q_idx) {
			  if(qseq[q_idx] > 0) {
				nonEmptyFlowFirst = q_idx;
				break;
			  }
			}
			int nonEmptyFlowLast = 0; // The index of the last non-empty read flow
			for(int q_idx = qseq.size()-1; q_idx >= 0; --q_idx) {
			  if(qseq[q_idx] > 0) {
				nonEmptyFlowLast = q_idx;
				break;
			  }
			}

			// Pretty print realignment results
			BasecallerRead bcRead;

			//reference-based
			bcRead.sequence.reserve(arg->key_by_read_group[read_group].length() + tseq_bases.length());
			copy(arg->key_by_read_group[read_group].begin(), arg->key_by_read_group[read_group].end(), back_inserter(bcRead.sequence));
			copy(tseq_bases.begin(), tseq_bases.end(), back_inserter(bcRead.sequence));
            position = position + tseq_bases.size();

			ion::FlowOrder fOrder(arg->flow_order_by_read_group[read_group], arg->flow_order_by_read_group[read_group].length());
			DPTreephaser dpTreephaser(fOrder);

            if(arg->skipDroop)
              dpTreephaser.SetModelParameters(zp_tag[0], zp_tag[1]);
            else
              dpTreephaser.SetModelParameters(zp_tag[0], zp_tag[1], zp_tag[2]);

			//only simulate if read is longer enough
			int numSimulatedFlows = first_useful_flow + tseq.size();
			if( numSimulatedFlows < 50) //ignore if less than 50 effective flows
			{
				sem_wait(&semLoadFull[arg->threadIndex]);			
				pthread_mutex_lock(&mutexQuit);
				myQuit = quit[arg->threadIndex];
				pthread_mutex_unlock(&mutexQuit);
				if(myQuit)
				{
					pthread_mutex_lock(&mutexNotDone);
					if(notDone[arg->threadIndex] > 0)
					{
						myQuit = false;
					}
					pthread_mutex_unlock(&mutexNotDone);
				}
				continue;
			}

			dpTreephaser.Simulate(bcRead, numSimulatedFlows);

/*			if(printAlign){
				for (int offset = 0; offset < (int)qseq.size(); offset += 150) {
				  int range = min(offset+150, (int)qseq.size());
				  printf ("Flow F% 4d: ", offset);
				  for (int idx = offset; idx < range; ++idx)
					printf("%c",flowOrder[idx]);
				  printf ("\nFlow Q% 4d: ", offset);
				  for (int idx = offset; idx < range; ++idx)
					printf("%d",qseq[idx]);
				  printf ("\nFlow A% 4d: ", offset);
				  for (int idx = offset; idx < range; ++idx)
					printf("%c",aln[idx]);
				  printf ("\nFlow T% 4d: ", offset);
				  for (int idx = offset; idx < range; ++idx)
					printf("%d",tseq[idx]);
				  printf ("\n\n");
				}
			}
*/
            int flowPosition = first_useful_flow;
            bool ignorePM = false;
            for (int pos = 0; pos < (int)qseq.size(); ++pos) {
			  if((aln[pos] == ALN_MATCH or aln[pos] == ALN_MISMATCH) and pos > nonEmptyFlowFirst and pos < nonEmptyFlowLast)
			  {
                  if(ignorePM || pos > (int)qseq.size() - 32) //ignore last 32 alignments for model fitting
                    arg->hptable->addAlignmentRecord(qseq[pos], tseq[pos], perturbation[pos], AlignmentInfo(refID, position), flowOrder[pos], flowPosition, x, y );
                  else
                    arg->hptable->addAlignmentRecord(qseq[pos], tseq[pos], perturbation[pos], bcRead.prediction[flowPosition], zm_tag[flowPosition] * 0.00390625, AlignmentInfo(refID, position), flowOrder[pos], flowPosition, x, y ); //zm_tag[flowPosition] * 0.00390625
			  }

              //update position
              if(isReversedStrand){
                position -= tseq[pos];
              } else{
                position += tseq[pos];
              }

			  //ignore reads having deletions
			  if(aln[pos] == ALN_DEL || aln[pos] == ALN_INS)
                ignorePM = true; //ignore portions containing insertions or deletions, which might be SNP related
              if (aln[pos] != ALN_DEL)
                flowPosition++;
			}

			++(*(arg->numAlignedReads)); 

            sem_wait(&semLoadFull[arg->threadIndex]);
			pthread_mutex_lock(&mutexQuit);
			myQuit = quit[arg->threadIndex];
			pthread_mutex_unlock(&mutexQuit);
			if(myQuit)
			{
				pthread_mutex_lock(&mutexNotDone);
				if(notDone[arg->threadIndex] > 0)
				{
					myQuit = false;
				}
				pthread_mutex_unlock(&mutexNotDone);
			}
		}
	}
		
	cout << "Recall thread " << arg->threadIndex << " exits." << endl;

	return NULL;
}


int main (int argc, const char *argv[])
{
  printf ("calibrate %s-%s (%s)\n", IonVersion::GetVersion().c_str(), IonVersion::GetRelease().c_str(), IonVersion::GetSvnRev().c_str());

  OptArgs opts;
  opts.ParseCmdLine(argc, argv);

  // bool printAlign   = opts.GetFirstBoolean('-', "printAlign", false);
  string output_dir = opts.GetFirstString ('o', "output-dir", ".");

  string rawFile    = opts.GetFirstString ('-', "rawFile", "");
  if(!rawFile.empty())
      rawFile = output_dir + "/" + rawFile;

  string hpModelFile  = opts.GetFirstString ('-', "hpModelFile", "hpModel.txt");
  if(!hpModelFile.empty())
      hpModelFile = output_dir + "/" + hpModelFile;

  string hpTableStratified = opts.GetFirstString ('-', "hpTableStratified", "flowQVtable.txt"); //be consistent with current production
  if(!hpTableStratified.empty())
      hpTableStratified = output_dir + "/" + hpTableStratified;

  string hpTableAggregated = opts.GetFirstString ('-', "hpTableAggregated", "");
  if(!hpTableAggregated.empty())
      hpTableAggregated = output_dir + "/" + hpTableAggregated;

  string alignmentAggregated = opts.GetFirstString ('-', "alignmentAggregated", "");
  if(!alignmentAggregated.empty())
      alignmentAggregated = output_dir + "/" + alignmentAggregated;

  string alignmentStratified = opts.GetFirstString ('-', "alignmentStratified", "");
  if(!alignmentStratified.empty())
      alignmentStratified = output_dir + "/" + alignmentStratified;

  string archiveFile = opts.GetFirstString ('-', "archiveFile", "HPTable.arc");
  if(!archiveFile.empty())
      archiveFile = output_dir + "/" + archiveFile;

  bool performMerge   = opts.GetFirstBoolean('-', "performMerge", false);

  bool hpmodelMerge = opts.GetFirstBoolean('-', "hpmodelMerge", false);

  bool skipDroop = opts.GetFirstBoolean('-', "skipDroop", false);


  if(performMerge){
    //load binary HPTables and merge it into one HPTable to support barcode runs
    string mergeParentDir = opts.GetFirstString ('-', "mergeParentDir", "");
    string commonName = opts.GetFirstString ('-', "commonName", "HPTable.arc");
    vector<string> arcFolders=vector<string>();
    DIR *dp;
    struct dirent *dirp;
    if((dp  = opendir(mergeParentDir.c_str())) == NULL) {
        cout << "Error(" << errno << ") opening " << mergeParentDir << endl;
        return errno;
    }

    while ((dirp = readdir(dp)) != NULL) {
      if(dirp->d_type == 0x4 and dirp->d_name[0] != '.')
         arcFolders.push_back(string(dirp->d_name));
    }
    closedir(dp);

    HPTable *mergedHPTable = 0;
    for(int ind = 0; ind < (int)arcFolders.size(); ++ind){
      string loadedFileName = mergeParentDir+"/"+arcFolders[ind]+"/"+commonName;
      HPTable *loadedHPTable = loadHPTable(loadedFileName.c_str());
      if(loadedHPTable != 0){
          if(mergedHPTable == 0){
            mergedHPTable = loadedHPTable;
          } else {
            mergedHPTable->addHPTable(loadedHPTable);
            delete loadedHPTable;
          }
      }
    }

    if(mergedHPTable == 0)
    {
      printf("HP table is not produced!\n");
      return 0;
    }

    mergedHPTable->process();

    if(!rawFile.empty())
      mergedHPTable->printRaw(rawFile.c_str());

    if(!hpModelFile.empty())
      mergedHPTable->printStratifiedModels(hpModelFile.c_str());

    if(!hpTableStratified.empty())
      mergedHPTable->printStratifiedTable(hpTableStratified.c_str());

    if(!hpTableAggregated.empty())
      mergedHPTable->printAggregatedTable(hpTableAggregated.c_str());

    if(!alignmentAggregated.empty())
      mergedHPTable->printAggregatedAlignSummary(alignmentAggregated.c_str());

    if(!alignmentStratified.empty())
      mergedHPTable->printStratifiedAlignSummary(alignmentStratified.c_str());

    delete mergedHPTable;

  } else if(hpmodelMerge){
      //load HPTables and merge it into one HPTable to produce fuse.hpmodel.txt
      string mergeParentDir = opts.GetFirstString ('-', "mergeParentDir", "./");
      vector<string> blockFolders=vector<string>();
      DIR *dp;
      struct dirent *dirp;
      if((dp  = opendir(mergeParentDir.c_str())) == NULL) {
          cout << "Error(" << errno << ") opening " << mergeParentDir << endl;
          return errno;
      }

      while ((dirp = readdir(dp)) != NULL) {
          if(dirp->d_type == 0x4 and dirp->d_name[0] != '.' and strcmp(dirp->d_name, "sigproc_results") != 0 and strcmp(dirp->d_name, "basecaller_results") != 0)
           blockFolders.push_back(string(dirp->d_name));
      }
      closedir(dp);

      string header = "";
      int xMin = 1000000;
      int xMax = 0;
      int yMin = 1000000;
      int yMax = 0;
      int xSpan = -1;
      int ySpan = -1;
      int flowStart = 1000000;
      int flowEnd = 0;
      int flowSpan = -1;
      int maxHPLength = -1;

      list<string> modelEntries = list<string>(); //no sorting operation, mainly serve as container

      for(int ind = 0; ind < (int)blockFolders.size(); ++ind){
        //open a file
        string hpModelFileName = mergeParentDir+"/"+blockFolders[ind]+"/basecaller_results/recalibration/hpModel.txt";
        ifstream model_file;
        model_file.open(hpModelFileName.c_str());
        //getHeader, append all model entries and update the ranges
        if (!model_file.fail()) {
            getline(model_file, header); //always overwrite the header, which is same across all hpModel files
            int flowStartLocal, flowEndLocal, flowSpanLocal, xMinLocal, xMaxLocal, xSpanLocal, yMinLocal, yMaxLocal, ySpanLocal, max_hp_calibratedLocal;
            model_file >> flowStartLocal >> flowEndLocal >> flowSpanLocal >> xMinLocal >> xMaxLocal >> xSpanLocal >> yMinLocal >> yMaxLocal >> ySpanLocal >>  max_hp_calibratedLocal;
            if(flowStart > flowStartLocal)
                flowStart = flowStartLocal;
            if(flowEnd < flowEndLocal)
                flowEnd = flowEndLocal;
            if(flowSpan == - 1)
                flowSpan = flowSpanLocal; //assuming flowSpanLocal would be same across blocks
            if(xMin > xMinLocal)
                xMin = xMinLocal;
            if(xMax < xMaxLocal)
                xMax = xMaxLocal;
            if(xSpan == - 1)
                xSpan = xSpanLocal;
            if(yMin > yMinLocal)
                yMin = yMinLocal;
            if(yMax < yMaxLocal)
                yMax = yMaxLocal;
            if(ySpan == - 1)
                ySpan = ySpanLocal;
            if(maxHPLength == -1)
                maxHPLength = max_hp_calibratedLocal;
            //add rest of lines to modelEntries; it is fine to store all lines into memory before final writeout
            while(model_file.good()){
              string line;
              getline(model_file, line);
              if(!line.empty())
                  modelEntries.push_back(line);
            }
        }
        model_file.close();
      }

      //only proceed if modelEntries is not empty
      if(modelEntries.size() == 0)
          return 0;

      //produce fuse.hpModel.txt
      string fuseModelFileName = mergeParentDir+"/basecaller_results/recalibration/hpModel.txt";;
      FILE * hpTableFile = fopen(fuseModelFileName.c_str(),"w");
      if(hpTableFile == 0){
          printf("%s does not exist", fuseModelFileName.c_str());
          return 1;
      }
      //print header
      fprintf(hpTableFile, "%s\n", header.c_str());
      fprintf(hpTableFile, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", flowStart, flowEnd, flowSpan, xMin, xMax, xSpan, yMin, yMax, ySpan, maxHPLength);
      for (std::list<string>::iterator it = modelEntries.begin(); it != modelEntries.end(); it++)
          fprintf(hpTableFile, "%s\n", it->c_str());
      fclose(hpTableFile);
  } else{
      string input_bam  = opts.GetFirstString ('i', "input", "");
      int xMin          = opts.GetFirstInt    ('-', "xMin", 0);
      int xMax          = opts.GetFirstInt    ('-', "xMax", 4000);
      int xCuts         = opts.GetFirstInt    ('-', "xCuts", 1);
      int yMin          = opts.GetFirstInt    ('-', "yMin", 0);
      int yMax          = opts.GetFirstInt    ('-', "yMax", 4000);
      int yCuts         = opts.GetFirstInt    ('-', "yCuts", 1);
      int numFlows      = opts.GetFirstInt    ('-', "numFlows", 2000);
      int flowCuts      = opts.GetFirstInt    ('-', "flowCuts", 1);
      int numHPs        = opts.GetFirstInt    ('-', "numHPs", 12);
      int numPerturbs   = opts.GetFirstInt    ('-', "numPerturbations", 99);
      int numThreads    = opts.GetFirstInt    ('-', "numThreads", DEFAULT_NUM_THREADS);

      if(numThreads > MAX_NUM_THREADS)
      {
          printf("WARNING: The input number of threads %d is greater than the maximum number of threads %d.\nThe number of threads is reset to %d.\n", numThreads, MAX_NUM_THREADS, MAX_NUM_THREADS);
          numThreads = MAX_NUM_THREADS;
      }
      if (input_bam.empty())
        return PrintHelp();

//      opts.CheckNoLeftovers();


      BamReader reader;
      if (!reader.Open(input_bam)) {
        fprintf(stderr, "calibrate: Failed to open input file %s\n", input_bam.c_str());
        return 1;
      }

      SamHeader sam_header = reader.GetHeader();

      map<string,string>  key_by_read_group;
      map<string,string>  flow_order_by_read_group;

      for (SamReadGroupIterator rg = sam_header.ReadGroups.Begin(); rg != sam_header.ReadGroups.End(); ++rg) {
        if(!rg->HasFlowOrder() or !rg->HasKeySequence()) {
          fprintf(stderr, "calibrate: Read group %s is missing flow order or key sequence\n", rg->ID.c_str());
          return 1;
        }
        key_by_read_group[rg->ID] = rg->KeySequence;
        flow_order_by_read_group[rg->ID] = rg->FlowOrder;
      }

      //HP Table initialization
      vector<shared_ptr<HPTable> > hpTableList;

      int numAlignedReads = 0;

          int threadIndex = 0;

          for(threadIndex = 0; threadIndex < numThreads; ++threadIndex)
          {
              hpTableList.push_back(make_shared<HPTable>(HPTable(xMin, xMax, xCuts, yMin, yMax, yCuts, numFlows, flowCuts, numPerturbs, numHPs)));
              sem_init(&semLoadNull[threadIndex], 0, 0);
              sem_init(&semLoadFull[threadIndex], 0, 0);
              sem_post(&semLoadNull[threadIndex]);
              quit[threadIndex] = false;
              notDone[threadIndex] = 0;
          }

          pthread_mutex_init(&mutexQuit, NULL);
          pthread_mutex_init(&mutexNotDone, NULL);

          loadArg argLoad;
          argLoad.numThreads = numThreads;
          argLoad.bamReader = &reader;
          pthread_t loadThread;
          pthread_create(&loadThread, NULL, LoadFunc, &argLoad);

          int nReads[MAX_NUM_THREADS];
          recallArg argRecall[MAX_NUM_THREADS];
          pthread_t recallThread[MAX_NUM_THREADS];
          for(threadIndex = 0; threadIndex < numThreads; ++threadIndex)
          {
              nReads[threadIndex] = 0;
              argRecall[threadIndex].threadIndex = threadIndex;
              argRecall[threadIndex].numAlignedReads = &(nReads[threadIndex]);
              argRecall[threadIndex].hptable = hpTableList[threadIndex].get();
              argRecall[threadIndex].skipDroop = skipDroop;
              argRecall[threadIndex].key_by_read_group = key_by_read_group;
              argRecall[threadIndex].flow_order_by_read_group = flow_order_by_read_group;

              pthread_create(&recallThread[threadIndex], NULL, RecallFunc, &argRecall[threadIndex]);
          }

          pthread_join(loadThread, NULL);
          for(int threadIndex = 0; threadIndex < numThreads; ++threadIndex)
          {
              pthread_join(recallThread[threadIndex], NULL);
          }

          reader.Close();

          for(threadIndex = 0; threadIndex < numThreads; ++threadIndex)
          {
              sem_destroy(&semLoadNull[threadIndex]);
              sem_destroy(&semLoadFull[threadIndex]);
          }

          pthread_mutex_destroy(&mutexQuit);
          pthread_mutex_destroy(&mutexNotDone);

          for(threadIndex = 0; threadIndex < numThreads; ++threadIndex)
          {
              numAlignedReads += nReads[threadIndex];
          }

      printf("Processing %d reads\n", numAlignedReads);

      //merge hptable
      HPTable* hpTablePtr = mergeHPTables(hpTableList);
      if(hpTablePtr == 0)
      {
        printf("HP table is not produced!\n");
        return 0;
      }

      //process hptable
      hpTablePtr->process();

      if(!rawFile.empty())
        hpTablePtr->printRaw(rawFile.c_str());

      if(!hpModelFile.empty())
        hpTablePtr->printStratifiedModels(hpModelFile.c_str());

      if(!hpTableStratified.empty())
        hpTablePtr->printStratifiedTable(hpTableStratified.c_str());

      if(!hpTableAggregated.empty())
        hpTablePtr->printAggregatedTable(hpTableAggregated.c_str());

      if(!alignmentAggregated.empty())
        hpTablePtr->printAggregatedAlignSummary(alignmentAggregated.c_str());

      if(!alignmentStratified.empty())
        hpTablePtr->printStratifiedAlignSummary(alignmentStratified.c_str());

      if(!archiveFile.empty())
        hpTablePtr->saveToFile(archiveFile.c_str());

      delete hpTablePtr;
  }
  return 0;
}

//TODO
//optimize 0 mer handling to speedup; way too many 0mer flow to process





