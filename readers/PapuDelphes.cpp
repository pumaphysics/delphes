#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <array>
#include <stdlib.h>
#include <functional>
#include <time.h>
#include <math.h>
#include <numeric>

#include "TROOT.h"

#include "TFile.h"
#include "TTree.h"
#include "TLeaf.h"
#include "TLorentzVector.h"
#include "TMath.h"

#include "ExRootAnalysis/ExRootProgressBar.h"
#include "ExRootAnalysis/ExRootTreeBranch.h"
#include "ExRootAnalysis/ExRootTreeWriter.h"

using namespace std;


//---------------------------------------------------------------------------

static int NMAX = 9000;

struct PFCand
{
  float pt = 0;
  float eta = 0;
  float phi = 0;
  float x = 0; // cos and sin phi
  float y = 0;
  float e = 0;
  float puppi = 1;
  float pdgid = 0;
  float hardfrac = 1;  
  float cluster_idx = -1;
  float cluster_hardch_pt = 0;
  float cluster_puch_pt = 0;
  float cluster_r = 0;
  float vtxid = -1;
  float npv = 0;
  float isolep = 0;
};


float xy_to_phi(float x, float y) {
  return TMath::ATan2(y, x);
  // float phi = TMath::ACos(x);
  // return (y < 0) ? -phi : phi;
}


class Cluster : public vector<PFCand*> {
public:
  void finalize () 
  {
    float _x = 0, _y = 0;

    for (auto* p : *this) {
      _sum_pt += p->pt;
      if (p->vtxid==0){
	_hardch_pt += p->pt;
	//std::cout << _hardch_pt << std::endl;
      }
      else if (p->vtxid==1){
	_puch_pt += p->pt;
      }
      _eta += p->pt * p->eta;
      _x += p->pt * p->x;
      _y += p->pt * p->y;
    }
    _eta /= _sum_pt;
    _x /= _sum_pt;
    _y /= _sum_pt;

    float r = sqrt(pow(_x, 2) + pow(_y, 2));
    _x /= r;
    _y /= r;

    //if (_hardch_pt>90)
    //std::cout << _hardch_pt << std::endl;

    float largest_dr = -1.;
    for (auto* p : *this) {
      auto dr = pow(_eta - p->eta, 2) + pow(_x - p->x, 2) + pow(_y - p->y, 2);
      if (dr > largest_dr)
	largest_dr = dr;
    }
    _r = largest_dr;

    _phi = xy_to_phi(_x, _y);
  }

  float eta() { return _eta; }
  float phi() { return _phi; }
  float sum_pt() { return _sum_pt; }
  float hardch_pt() { return _hardch_pt; }
  float puch_pt() { return _puch_pt; }
  float r() { return _r; }

private:
  float _eta=0, _phi=0, _sum_pt=0, _hardch_pt=0, _puch_pt=0, _r=0;

};


template<int K>
class KMeans { 
public:
    KMeans(vector<PFCand*> particles, int max_iter=20) 
    {
        // randomly initialize centroids
        array<int, K> i_centroids;
        for (int i=0; i!=K; ++i) {
            while (true) {
                int i_p = rand() % particles.size();
                bool found = false;
                for (int j=0; j!=i; ++j) {
                    found = (i_centroids[j] == i_p);
                    if (found)
                        break;
                }
                if (!found) {
                    i_centroids[i] = i_p;
                    centroids[i][0] = particles[i_p]->eta;
                    centroids[i][1] = particles[i_p]->x;
                    centroids[i][2] = particles[i_p]->y;
                    break;
                }
            }
        } 

        for (int i_iter=0; i_iter!=max_iter; ++i_iter) {
            assign_particles(particles);
            update_centroids();
        }
    }

    ~KMeans() { }

    const array<Cluster, K> get_clusters() { return clusters; }

private:
    array<array<float, 3>, K> centroids;
    array<Cluster, K> clusters;

    void assign_particles(vector<PFCand*> &particles) 
    {
        for (int i=0; i!=K; ++i) {
            clusters[i].clear();
        }

        for (auto& p : particles) {
            float closest = 99999;
            int i_closest = -1;
            float eta = p->eta; float x = p->x; float y = p->y;

            for (int i=0; i!=K; ++i) {
                auto dr = pow(eta - centroids[i][0], 2) 
			  + pow(x - centroids[i][1], 2)
			  + pow(y - centroids[i][2], 2);
                if (dr < closest) {
                    closest = dr;
                    i_closest = i;
                }
            }
            clusters[i_closest].push_back(p);
        }
    }

    void update_centroids() 
    {
        for (int i=0; i!=K; ++i) {
            float eta_sum=0, x_sum=0, y_sum=0;
            auto &cluster = clusters[i];
            for (auto& p : cluster) {
                eta_sum += p->eta;
                x_sum += p->x;
                y_sum += p->y;
            }

	    x_sum /= cluster.size();
	    y_sum /= cluster.size();

	    float r = sqrt(pow(x_sum, 2) + pow(y_sum, 2));
	    x_sum /= r;
	    y_sum /= r;

            centroids[i][0] = eta_sum / cluster.size();
            centroids[i][1] = x_sum;
            centroids[i][2] = y_sum;
        }
    }
}; 




template <int K, int N>
class HierarchicalOrdering {
public:
    HierarchicalOrdering(int max_depth=-1): _max_depth(max_depth) { }

    vector<Cluster> 
    fit(vector<PFCand> &particles)
    {
        vector<PFCand*> p_particles;
        for (auto& p : particles)
            p_particles.push_back(&p);

        auto clusters = _recursive_fit(p_particles, 0);
        for (auto& c : clusters)
          c.finalize();
        return clusters;
    }
    
private:
    vector<Cluster> 
    _recursive_fit(const vector<PFCand*> &particles, int depth)
    {
        vector<Cluster> clusters;

        auto kmeans = KMeans<K>(particles);
        for (int i_k=0; i_k!=K; ++i_k) {
            auto cluster = kmeans.get_clusters()[i_k];
            if (cluster.size() > N && depth != _max_depth) {
                auto split_clusters = _recursive_fit(cluster, depth+1);
                for (auto& c : split_clusters) {
                    clusters.push_back(c);
                }
            } else {
                clusters.push_back(cluster);
            } 
        }
        return clusters;
    }

    int _max_depth;
};

template <typename T>
void 
fill(vector<float> &vattr, vector<PFCand> &particles, T fn_attr)
{
  vattr.clear();
  for (auto& p : particles)
    vattr.push_back(fn_attr(p));
}


//---------------------------------------------------------------------------

int main(int argc, char *argv[])
{

  srand(time(NULL));
  //srand(777);

  if(argc < 3) {
    cout << " Usage: " << "PapuDelphes" << " input_file"
         << " output_file" << endl;
    cout << " input_file - input file in ROOT format," << endl;
    cout << " output_file - output file in ROOT format" << endl;
    return 1;
  }

  // figure out how to read the file here 
  //

  TFile* ifile = TFile::Open(argv[1], "READ");
  TTree* itree = (TTree*)ifile->Get("Delphes;1");

  auto* fout = TFile::Open(argv[2], "RECREATE");
  auto* tout = new TTree("events", "events");

  unsigned int nevt = itree->GetEntries();
  TBranch* pfbranch = (TBranch*)itree->GetBranch("ParticleFlowCandidate");
  TBranch* genjetbranch = (TBranch*)itree->GetBranch("GenJet");
  TBranch* genbranch = (TBranch*)itree->GetBranch("PileUpMix");
  TBranch* electronbranch = (TBranch*)itree->GetBranch("Electron");
  TBranch* muonbranch = (TBranch*)itree->GetBranch("MuonLoose");
  TBranch* zbranch = (TBranch*)itree->GetBranch("ZBoson");
  std::cout << "NEVT: " << nevt << std::endl;
  vector<PFCand> input_particles;

  vector<PFCand> output_particles;
  output_particles.reserve(NMAX);

  vector<float> vpt, veta, vphi, ve, vpuppi, vpdgid, vhardfrac, vcluster_idx, vvtxid, vcluster_r, vcluster_hardch_pt, vcluster_puch_pt, vnpv, visolep;
  vpt.reserve(NMAX); veta.reserve(NMAX); vphi.reserve(NMAX); 
  ve.reserve(NMAX); vpuppi.reserve(NMAX); vpdgid.reserve(NMAX); 
  vhardfrac.reserve(NMAX); vcluster_idx.reserve(NMAX); vvtxid.reserve(NMAX);
  vcluster_r.reserve(NMAX); vcluster_hardch_pt.reserve(NMAX); vcluster_puch_pt.reserve(NMAX);
  vnpv.reserve(NMAX);
  visolep.reserve(NMAX);

  float genmet=-99., genmetphi=-99., genUmag=-99., genUphi=-99.;
  float genZpt=-99., genZeta=-99., genZphi=-99., genZm=-99., genZacc=-99.;
  float recZpt=-99., recZeta=-99., recZphi=-99., recZm=-99.;
  float genjet1pt=-99., genjet1eta=-99., genjet1phi=-99., genjet1e=-99.;
  float genjet2pt=-99., genjet2eta=-99., genjet2phi=-99., genjet2e=-99.;
  
  tout->Branch("pt", &vpt);
  tout->Branch("eta", &veta);
  tout->Branch("phi", &vphi);
  tout->Branch("e", &ve);
  tout->Branch("puppi", &vpuppi);
  tout->Branch("pdgid", &vpdgid);
  tout->Branch("hardfrac", &vhardfrac);
  tout->Branch("cluster_idx", &vcluster_idx);
  tout->Branch("cluster_r", &vcluster_r);
  tout->Branch("cluster_hardch_pt", &vcluster_hardch_pt);
  tout->Branch("cluster_puch_pt", &vcluster_puch_pt);
  tout->Branch("vtxid", &vvtxid);
  tout->Branch("npv", &vnpv);
  tout->Branch("isolep", &visolep);
  
  TBranch* b_genZacc = tout->Branch("genZacc",&genZacc, "genZacc/F");
  TBranch* b_genZpt = tout->Branch("genZpt",&genZpt, "genZpt/F");
  TBranch* b_genZeta = tout->Branch("genZeta",&genZeta, "genZeta/F");
  TBranch* b_genZphi = tout->Branch("genZphi",&genZphi, "genZphi/F");
  TBranch* b_genZm = tout->Branch("genZm",&genZm, "genZm/F");
  TBranch* b_recZpt = tout->Branch("recZpt",&recZpt, "recZpt/F");
  TBranch* b_recZeta = tout->Branch("recZeta",&recZeta, "recZeta/F");
  TBranch* b_recZphi = tout->Branch("recZphi",&recZphi, "recZphi/F");
  TBranch* b_recZm = tout->Branch("recZm",&recZm, "recZm/F");
  TBranch* b_genmet = tout->Branch("genmet",&genmet, "genmet/F");
  TBranch* b_genmetphi = tout->Branch("genmetphi",&genmetphi, "genmetphi/F");
  TBranch* b_genUmag = tout->Branch("genUmag",&genUmag, "genUmag/F");
  TBranch* b_genUphi = tout->Branch("genUphi",&genUphi, "genUphi/F");
  TBranch* b_genjet1pt = tout->Branch("genjet1pt",&genjet1pt, "genjet1pt/F");
  TBranch* b_genjet1eta = tout->Branch("genjet1eta",&genjet1eta, "genjet1eta/F");
  TBranch* b_genjet1phi = tout->Branch("genjet1phi",&genjet1phi, "genjet1phi/F");
  TBranch* b_genjet1e = tout->Branch("genjet1e",&genjet1e, "genjet1e/F");
  TBranch* b_genjet2pt = tout->Branch("genjet2pt",&genjet2pt, "genjet2pt/F");
  TBranch* b_genjet2eta = tout->Branch("genjet2eta",&genjet2eta, "genjet2eta/F");
  TBranch* b_genjet2phi = tout->Branch("genjet2phi",&genjet2phi, "genjet2phi/F");
  TBranch* b_genjet2e = tout->Branch("genjet2e",&genjet2e, "genjet2e/F");

  auto ho = HierarchicalOrdering<4, 10>();
  //auto ho = HierarchicalOrdering<4, 20>();
  //auto ho = HierarchicalOrdering<4, 30>();

  ExRootProgressBar progressBar(nevt);
  
  auto comp_pt = [](auto &a, auto &b) { return a.sum_pt() > b.sum_pt(); };
  auto comp_p4 = [](auto &a, auto &b) { return a.pt > b.pt; };

  for (unsigned int k=0; k<nevt; k++){
    itree->GetEntry(k);
    
    float npv = itree->GetLeaf("Vertex_size")->GetValue(0);
    genmet = itree->GetLeaf("GenMissingET.MET")->GetValue(0);
    genmetphi = itree->GetLeaf("GenMissingET.Phi")->GetValue(0);

    // Hadronic recoil
    TVector2 vMet; vMet.SetMagPhi(genmet,genmetphi);
    unsigned int ngens = genbranch->GetEntries();
    ngens = itree->GetLeaf("PileUpMix_size")->GetValue(0);

    for (unsigned int j=0; j<ngens; j++){
      if (itree->GetLeaf("PileUpMix.PT")->GetValue(j)>10 && itree->GetLeaf("PileUpMix.IsPU")->GetValue(j)==0 && (abs(itree->GetLeaf("PileUpMix.PID")->GetValue(j))==11 || abs(itree->GetLeaf("PileUpMix.PID")->GetValue(j))==13)){
	TVector2 vLep; vLep.SetMagPhi(itree->GetLeaf("PileUpMix.PT")->GetValue(j),itree->GetLeaf("PileUpMix.Phi")->GetValue(j));
	vMet += vLep;
      }
    }

    genUmag = vMet.Mod();
    genUphi = vMet.Phi();

    // Z Boson

    TLorentzVector vZ(0,0,0,0);
    int firstpid = 0;
    bool bothfound = false;
    bool bothleptonsinacc = true;    

    /*
    for (unsigned int j=0; j<ngens; j++){
      if (bothfound)
	break;
      if (firstpid == 0 && itree->GetLeaf("PileUpMix.IsPU")->GetValue(j)==0 && (abs(itree->GetLeaf("PileUpMix.PID")->GetValue(j))==11 || abs(itree->GetLeaf("PileUpMix.PID")->GetValue(j))==13)){
	firstpid = itree->GetLeaf("PileUpMix.PID")->GetValue(j);
	TLorentzVector tmp; tmp.SetPtEtaPhiE(itree->GetLeaf("PileUpMix.PT")->GetValue(j),itree->GetLeaf("PileUpMix.Eta")->GetValue(j),itree->GetLeaf("PileUpMix.Phi")->GetValue(j),itree->GetLeaf("PileUpMix.E")->GetValue(j));
	vZ += tmp;
	if (abs(itree->GetLeaf("PileUpMix.Eta")->GetValue(j))>4.0)
	  bothleptonsinacc = false;	
      }
      if (firstpid == (-1)*itree->GetLeaf("PileUpMix.PID")->GetValue(j) && itree->GetLeaf("PileUpMix.IsPU")->GetValue(j)==0 && (abs(itree->GetLeaf("PileUpMix.PID")->GetValue(j))==11 || abs(itree->GetLeaf("PileUpMix.PID")->GetValue(j))==13)){
	TLorentzVector tmp; tmp.SetPtEtaPhiE(itree->GetLeaf("PileUpMix.PT")->GetValue(j),itree->GetLeaf("PileUpMix.Eta")->GetValue(j),itree->GetLeaf("PileUpMix.Phi")->GetValue(j),itree->GetLeaf("PileUpMix.E")->GetValue(j));
	vZ += tmp;
	bothfound = true;
	if (abs(itree->GetLeaf("PileUpMix.Eta")->GetValue(j))>4.0)
	  bothleptonsinacc = false;
      }
    }
    */
    //vZ.SetPtEtaPhiM(itree->GetLeaf("ZBoson.PT")->GetValue(0),itree->GetLeaf("ZBoson.Eta")->GetValue(0),itree->GetLeaf("ZBoson.Phi")->GetValue(0),itree->GetLeaf("ZBoson.Mass")->GetValue(0));
    //cout <<  vZ.Pt() << endl;
    //cout << itree->GetLeaf("ZBoson.PT")->GetValue(0) << endl;
    genZpt = itree->GetLeaf("ZBoson.PT")->GetValue(0);
    genZeta = itree->GetLeaf("ZBoson.Eta")->GetValue(0);
    genZphi = itree->GetLeaf("ZBoson.Phi")->GetValue(0);
    genZm = itree->GetLeaf("ZBoson.Mass")->GetValue(0);
    //std::cout << "Dilep mass: " << vZ.M() << std::endl;
    if (bothleptonsinacc)
      genZacc = 1.;
    else 
      genZacc = 0.;


    TLorentzVector vrecZ(0,0,0,0);
    int firstrecpid = 0;
    bool bothrecfound = false;    

    unsigned int nelectron = electronbranch->GetEntries();
    nelectron = itree->GetLeaf("Electron_size")->GetValue(0);
    unsigned int nmuon = muonbranch->GetEntries();
    nmuon = itree->GetLeaf("MuonLoose_size")->GetValue(0);

    std::vector<float> leptonpt;
    //int leptype = 0;

    /*
    for (unsigned int j=0; j<nelectron; j++){
      leptonpt.push_back(itree->GetLeaf("Electron.PT")->GetValue(j));
    }
    for (unsigned int j=0; j<nmuon; j++){
      leptonpt.push_back(itree->GetLeaf("MuonLoose.PT")->GetValue(j));
    }
    */

    float maxleppt = -99;
    bool maxisele = 0;
    if (nelectron>1){
      maxleppt = itree->GetLeaf("Electron.PT")->GetValue(0);
      maxisele = 1;
    }
    if (nmuon>1){
      if (itree->GetLeaf("MuonLoose.PT")->GetValue(0)>maxleppt){
	maxleppt = itree->GetLeaf("MuonLoose.PT")->GetValue(0);
	maxisele = 0;
      }
    }

    if (maxleppt>0){
      if (maxisele){
	for (unsigned int j=0; j<nelectron; j++){
	  if (bothrecfound)
	    break;
	  if (firstrecpid == 0 && itree->GetLeaf("Electron.PT")->GetValue(j)>10){
	    firstrecpid = itree->GetLeaf("Electron.Charge")->GetValue(j);
	    TLorentzVector tmp; tmp.SetPtEtaPhiM(itree->GetLeaf("Electron.PT")->GetValue(j),itree->GetLeaf("Electron.Eta")->GetValue(j),itree->GetLeaf("Electron.Phi")->GetValue(j),0.00051099);
	    vrecZ += tmp;
	    leptonpt.push_back(itree->GetLeaf("Electron.PT")->GetValue(j));
	  }
	  if (firstrecpid == (-1)*itree->GetLeaf("Electron.Charge")->GetValue(j) && itree->GetLeaf("Electron.PT")->GetValue(j)>10){
	    TLorentzVector tmp; tmp.SetPtEtaPhiM(itree->GetLeaf("Electron.PT")->GetValue(j),itree->GetLeaf("Electron.Eta")->GetValue(j),itree->GetLeaf("Electron.Phi")->GetValue(j),0.00051099);
	    vrecZ += tmp;
	    leptonpt.push_back(itree->GetLeaf("Electron.PT")->GetValue(j));
	    //std::cout << "Dilep rec mass: " << vrecZ.M() << std::endl;
	    bothrecfound = true;
	  }
	}
      }
      else{
	for (unsigned int j=0; j<nmuon; j++){
	  if (bothrecfound)
	    break;
	  if (firstrecpid == 0 && itree->GetLeaf("MuonLoose.PT")->GetValue(j)>10){
	    firstpid = itree->GetLeaf("MuonLoose.Charge")->GetValue(j);
	    TLorentzVector tmp; tmp.SetPtEtaPhiM(itree->GetLeaf("MuonLoose.PT")->GetValue(j),itree->GetLeaf("MuonLoose.Eta")->GetValue(j),itree->GetLeaf("MuonLoose.Phi")->GetValue(j),0.1057);
	    vrecZ += tmp;
	    leptonpt.push_back(itree->GetLeaf("MuonLoose.PT")->GetValue(j));
	  }
	  if (firstrecpid == (-1)*itree->GetLeaf("MuonLoose.Charge")->GetValue(j) && itree->GetLeaf("MuonLoose.PT")->GetValue(j)>10){
	    TLorentzVector tmp; tmp.SetPtEtaPhiM(itree->GetLeaf("MuonLoose.PT")->GetValue(j),itree->GetLeaf("MuonLoose.Eta")->GetValue(j),itree->GetLeaf("MuonLoose.Phi")->GetValue(j),0.1057);
	    vrecZ += tmp;
	    leptonpt.push_back(itree->GetLeaf("MuonLoose.PT")->GetValue(j));
	    //std::cout << "Dilep rec mass: " << vrecZ.M() << std::endl;
	    bothrecfound = true;
	  }
	}
      }      
    }

    recZpt = vrecZ.Pt();
    recZeta = vrecZ.Eta();
    recZphi = vrecZ.Phi();
    recZm = vrecZ.M();
    
    unsigned int ngenjets = genjetbranch->GetEntries();
    ngenjets = itree->GetLeaf("GenJet_size")->GetValue(0);

    for (unsigned int j=0; j<ngenjets; j++){
      if (j>1)
	break;
      TLorentzVector tmpjet;
      tmpjet.SetPtEtaPhiM(itree->GetLeaf("GenJet.PT")->GetValue(j),itree->GetLeaf("GenJet.Eta")->GetValue(j),itree->GetLeaf("GenJet.Phi")->GetValue(j),itree->GetLeaf("GenJet.Mass")->GetValue(j));
      if (j==0){
	genjet1pt = tmpjet.Pt();
	genjet1eta = tmpjet.Eta();
	genjet1phi = tmpjet.Phi();
	genjet1e = tmpjet.E();
      }
      if (j==1){
	genjet2pt = tmpjet.Pt();
	genjet2eta = tmpjet.Eta();
	genjet2phi = tmpjet.Phi();
	genjet2e = tmpjet.E();
      }      
    }

    input_particles.clear();
    unsigned int npfs = pfbranch->GetEntries();
    npfs = itree->GetLeaf("ParticleFlowCandidate_size")->GetValue(0);
    for (unsigned int j=0; j<npfs; j++){
      PFCand tmppf;
      tmppf.npv = npv;
      tmppf.pt = itree->GetLeaf("ParticleFlowCandidate.PT")->GetValue(j);
      tmppf.eta = itree->GetLeaf("ParticleFlowCandidate.Eta")->GetValue(j);
      tmppf.phi = itree->GetLeaf("ParticleFlowCandidate.Phi")->GetValue(j);
      tmppf.x = TMath::Cos(tmppf.phi);
      tmppf.y = TMath::Sin(tmppf.phi);
      tmppf.e = itree->GetLeaf("ParticleFlowCandidate.E")->GetValue(j);
      tmppf.puppi = itree->GetLeaf("ParticleFlowCandidate.PuppiW")->GetValue(j);
      tmppf.hardfrac = itree->GetLeaf("ParticleFlowCandidate.hardfrac")->GetValue(j);
      tmppf.pdgid = itree->GetLeaf("ParticleFlowCandidate.PID")->GetValue(j);
      if (itree->GetLeaf("ParticleFlowCandidate.Charge")->GetValue(j)!=0){
	if (itree->GetLeaf("ParticleFlowCandidate.hardfrac")->GetValue(j)==1)
	  tmppf.vtxid = 0;
	else
	  tmppf.vtxid = 1;
      }
      else
	tmppf.vtxid = -1;
      if ((abs(tmppf.pdgid)==11 || abs(tmppf.pdgid)==13) && tmppf.pt>10){
	std::vector<float>::iterator it;
	it = std::find (leptonpt.begin(), leptonpt.end(), tmppf.pt);
	if (it != leptonpt.end())
	  tmppf.isolep = 1;  
	else
	  tmppf.isolep = 0;  
      }
      input_particles.push_back(tmppf);
    }

    // sorting input particles by pT
    sort(input_particles.begin(), input_particles.end(), comp_p4);    

    // get clusters of 10 particles
    auto clusters = ho.fit(input_particles);

    // sort clusters by sum pT. not very efficient since we recompute
    // sum_pt for each comparison but whatever 
    sort(clusters.begin(), clusters.end(), comp_pt);

    // now sort clusters by proximity to last cluster, starting with
    // the hardest cluster
    vector<Cluster> sorted_clusters = {clusters[0]};
    clusters.erase(clusters.begin());
    while (clusters.size() > 0) {
      unsigned i_cluster = 0;
      float best_dr2 = 999999;
      auto &last_cluster = sorted_clusters.back();
      for (unsigned j=0; j!=clusters.size(); ++j) {
        float dr2 = pow(last_cluster.eta() - clusters[j].eta(), 2)
                    + pow(last_cluster.phi() - clusters[j].phi(), 2);
        if (dr2 < best_dr2) {
          i_cluster = j;
          best_dr2 = dr2;
        }
      }
      sorted_clusters.push_back(clusters[i_cluster]);
      clusters.erase(clusters.begin()+i_cluster);
    }

    output_particles.clear();
    int cluster_idx = 0;
    for (auto& cluster : sorted_clusters) {
      for (auto* p : cluster) {
        p->cluster_idx = cluster_idx;
	p->cluster_hardch_pt = cluster.hardch_pt();
	p->cluster_puch_pt = cluster.puch_pt();
	p->cluster_r = cluster.r();

	//if (p->cluster_hardch_pt>90)
	//std::cout << "WTF: " << p->cluster_hardch_pt << std::endl;
	//std::cout << "Hard: " << p->cluster_hardch_pt << std::endl;
	//std::cout << "PU: " << p->cluster_puch_pt << std::endl;
	
        output_particles.push_back(*p); 
      }
      ++cluster_idx;
    }
    // if there are fewer than NMAX, it'll get padded out with default values
    output_particles.resize(NMAX);

    fill(vpt, output_particles, [](PFCand& p) { return p.pt; }); 
    fill(veta, output_particles, [](PFCand& p) { return p.eta; }); 
    fill(vphi, output_particles, [](PFCand& p) { return p.phi; }); 
    fill(ve, output_particles, [](PFCand& p) { return p.e; }); 
    fill(vpuppi, output_particles, [](PFCand& p) { return p.puppi; }); 
    fill(vpdgid, output_particles, [](PFCand& p) { return p.pdgid; }); 
    fill(vhardfrac, output_particles, [](PFCand& p) { return p.hardfrac; }); 
    fill(vcluster_idx, output_particles, [](PFCand& p) { return p.cluster_idx; }); 
    fill(vcluster_r, output_particles, [](PFCand& p) { return p.cluster_r; }); 
    fill(vcluster_hardch_pt, output_particles, [](PFCand& p) { return p.cluster_hardch_pt; }); 
    fill(vcluster_puch_pt, output_particles, [](PFCand& p) { return p.cluster_puch_pt; }); 
    fill(vvtxid, output_particles, [](PFCand& p) { return p.vtxid; }); 
    fill(vnpv, output_particles, [](PFCand& p) { return p.npv; }); 
    fill(visolep, output_particles, [](PFCand& p) { return p.isolep; }); 
    
    tout->Fill();

    progressBar.Update(k, k);

  }

  fout->Write();
  fout->Close();

}
