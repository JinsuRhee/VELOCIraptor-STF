/*! \file mpiroutines.cxx
 *  \brief this file contains routines used with MPI compilation. 

    MPI routines generally pertain to domain decomposition or to specific MPI tasks that determine what needs to be broadcast between various threads.

 */

#ifdef USEMPI

//-- For MPI

#include "halomergertree.h"

/// \name For MPI 
//@{
int ThisTask,NProcs;
int NSnap,StartSnap,EndSnap;
int *mpi_startsnap,*mpi_endsnap;
//@}


///load balance how snapshots are split across mpi domains
void MPILoadBalanceSnapshots(Options &opt){
    mpi_startsnap=new int[NProcs];
    mpi_endsnap=new int[NProcs];
    //if there is only one mpi thread no splitting to be done.
    if (NProcs==1) {
        StartSnap=0;
        EndSnap=opt.numsnapshots;
        NSnap=opt.numsnapshots;
        return;
    }

    ///determine the total number of haloes and ideal splitting
    if (ThisTask==0) {
        //there are two ways to load balance the data, halos or particles in haloes 
        //load balancing via particles is the defaul option but could compile with halo load balancing
        Int_t *numinfo=new Int_t[opt.numsnapshots];
#ifdef MPIHALOBALANCE
        cout<<"Halo based splitting"<<endl;
        Int_t totpart=ReadNumberofHalos(opt,numinfo);
#else
        cout<<"Particle in halos based splitting"<<endl;
        Int_t totpart=ReadNumberofParticlesInHalos(opt,numinfo);
#endif
        //now number of particles per mpi thread
        if (opt.numpermpi==0) opt.numpermpi=totpart/NProcs;
        Int_t sum=0, itask=NProcs-1, inumsteps=-1;
        mpi_endsnap[itask]=opt.numsnapshots;
        cout<<" Total number of items is "<<totpart<<" and should have "<<opt.numpermpi<<" per mpi"<<endl;
        for (int i=opt.numsnapshots-1;i>=0;i--) {
            sum+=numinfo[i];
            inumsteps++;
            if (sum>opt.numpermpi && inumsteps>=opt.numsteps) {
                mpi_startsnap[itask]=i;
                mpi_endsnap[itask-1]=i+opt.numsteps;
                itask--;
                //inumsteps=-1;
                //sum=0;
                inumsteps=opt.numsteps-1;
                sum=0;for (int j=0;j<=opt.numsteps;j++) sum+=numinfo[i+j];
                if (itask==0) break;
            }
        }
        mpi_startsnap[itask]=0;
        //if there too many mpi threads then some tasks might be empty with no work
        if (itask!=0) {
            cerr<<"Some MPI theads have no work: number of threads with no work "<<itask+1<<" out of "<<NProcs<<" running "<<endl;
            cerr<<"Exiting, adjust to "<<NProcs-itask-1<<" number of mpi theads "<<endl;MPI_Abort(MPI_COMM_WORLD,1);
        }
        int itaskworkload=0;
        Double_t maxworkload=0,minworkload=1e32;
        for (itask=0;itask<NProcs;itask++) 
        {
            sum=0;
            for (int i=mpi_startsnap[itask];i<mpi_endsnap[itask];i++) sum+=numinfo[i];
            cout<<itask<<" will use snaps "<<mpi_startsnap[itask]<<" to "<<mpi_endsnap[itask]-1<<" with overlap of "<<opt.numsteps<<" that contains "<<sum<<" item"<<endl;
            if (sum>maxworkload) maxworkload=sum;
            if (sum<minworkload) minworkload=sum;
        }
        delete[] numinfo;
        if (maxworkload/minworkload>MPILOADBALANCE) {
            cerr<<"MPI theads number + number of desired steps less than optimal, load imbalance is "<<maxworkload/minworkload<<endl;
            if (maxworkload/minworkload>=2.0*MPILOADBALANCE) {cerr<<"Exiting, adjust number of threads or number of items per thread to reduce load imbalance"<<endl;MPI_Abort(MPI_COMM_WORLD,1);}
        }
    }
    //task 0 has finished determining which snapshots a given mpi thread will process

    MPI_Bcast(mpi_startsnap, NProcs, MPI_Int_t, 0, MPI_COMM_WORLD);
    MPI_Bcast(mpi_endsnap, NProcs, MPI_Int_t, 0, MPI_COMM_WORLD);
    StartSnap=mpi_startsnap[ThisTask];
    EndSnap=mpi_endsnap[ThisTask];
    NSnap=EndSnap-StartSnap;
}

///mpi routine to broadcast the progenitor based descendant list for snapshots that overlap between mpi tasks. Ensures that descendent lists is complete and 
///can be used to clean progenitor list
void MPIUpdateProgenitorsUsingDescendants(Options &opt, HaloTreeData *&pht, DescendantDataProgenBased **&pprogendescen, ProgenitorData **&pprogen)
{
    if (opt.iverbose>0 && ThisTask==0) cout<<"Updating descendant based progenitor list across MPI threads"<<endl;
    ///broadcast overlaping snapshots
    int sendtask,recvtask, isnap;
    MPI_Status status;
    int nsendup,nsenddown;
    int mpi_nsendup[NProcs], mpi_nsenddown[NProcs];
    int sendupnumstep[NProcs], senddownnumstep[NProcs];
    int mpi_sendupnumstep[NProcs*NProcs], mpi_senddownnumstep[NProcs*NProcs];

    ///data structures used to store the data that is parsed using an offset pointer
    Int_t totitems;
    int *numdescen;
    long unsigned *noffset;
    long unsigned *tothaloindex;
    long unsigned *tothalotemporalindex;
    Double_t *totmerit;
    int *totdeltat;
    int *totMPITask;

    //first determine snapshot overlap
    nsendup=nsenddown=0;
    for (int itask=0;itask<NProcs;itask++) sendupnumstep[itask]=senddownnumstep[itask]=0;
    for (int itask=ThisTask+1;itask<NProcs;itask++) if (mpi_startsnap[itask]<EndSnap) {nsendup++;sendupnumstep[itask]=EndSnap-mpi_startsnap[itask];}
    for (int itask=ThisTask-1;itask>=0;itask--) if (mpi_endsnap[itask]>StartSnap) {nsenddown++;senddownnumstep[itask]=mpi_endsnap[itask]-StartSnap;}
    MPI_Allgather(&nsendup, 1, MPI_INT, mpi_nsendup, 1, MPI_INT, MPI_COMM_WORLD);
    MPI_Allgather(&nsenddown, 1, MPI_INT, mpi_nsenddown, 1, MPI_INT, MPI_COMM_WORLD);
    MPI_Allgather(sendupnumstep, NProcs, MPI_INT, mpi_sendupnumstep, NProcs, MPI_INT, MPI_COMM_WORLD);
    MPI_Allgather(senddownnumstep, NProcs, MPI_INT, mpi_senddownnumstep, NProcs, MPI_INT, MPI_COMM_WORLD);


    //then send information from task itask to itask+1, itask+2 ... for all snapshots that have overlapping times
    //note that as the DescendantDataProgenBased contains vectors, have to send size then each element which itself contains arrays
    //ideally we could use boost to specify the mpi data format but for now lets just generate arrays containing all the data that can then be parsed
    ///\todo need to clean up send style so that processors are waiting to do nothing
    ///plus there are issues with the buffer with this type of send style. Might need to be more clever
    for (int itask=0;itask<NProcs-1;itask++) {
    for (int jtask=1;jtask<=mpi_nsendup[itask];jtask++) {
//    for (int i=1;i<opt.numsteps;i++) {
    for (int i=1;i<=mpi_sendupnumstep[itask*NProcs+itask+jtask];i++) {
        //sending information
        if (itask==ThisTask) {
            recvtask=ThisTask+jtask;
            isnap=EndSnap-i;
            //build data structures to be sent
            totitems=0;
            for (Int_t j=0;j<pht[isnap].numhalos;j++) {
                totitems+=pprogendescen[isnap][j].NumberofDescendants;
            }
            MPI_Send(&totitems,1, MPI_Int_t, recvtask, isnap*NProcs*NProcs+ThisTask+NProcs, MPI_COMM_WORLD);
            if (totitems>0) {
                numdescen=new int[pht[isnap].numhalos];
                noffset=new long unsigned[pht[isnap].numhalos];
                tothaloindex=new long unsigned[totitems];
                tothalotemporalindex=new long unsigned[totitems];
                totmerit=new Double_t[totitems];
                totdeltat=new int[totitems];
                totMPITask=new int[totitems];

                totitems=0;
                for (Int_t j=0;j<pht[isnap].numhalos;j++) {
                    numdescen[j]=pprogendescen[isnap][j].NumberofDescendants;
                    noffset[j]=totitems;
                    for (Int_t k=0;k<numdescen[j];k++) {
                        tothaloindex[noffset[j]+k]=pprogendescen[isnap][j].haloindex[k];
                        tothalotemporalindex[noffset[j]+k]=pprogendescen[isnap][j].halotemporalindex[k];
                        totmerit[noffset[j]+k]=pprogendescen[isnap][j].Merit[k];
                        totdeltat[noffset[j]+k]=pprogendescen[isnap][j].deltat[k];
                        totMPITask[noffset[j]+k]=pprogendescen[isnap][j].MPITask[k];
                    }
                    totitems+=pprogendescen[isnap][j].NumberofDescendants;
                }

                //then send number of halos, total number of items and then the offset data so that the combined data can be parsed
                MPI_Send(&numdescen[0],pht[isnap].numhalos, MPI_INT, recvtask, isnap*NProcs*NProcs+ThisTask+2*NProcs, MPI_COMM_WORLD);
                MPI_Send(&noffset[0],pht[isnap].numhalos, MPI_LONG, recvtask, isnap*NProcs*NProcs+ThisTask+3*NProcs, MPI_COMM_WORLD);
                MPI_Send(&tothaloindex[0],totitems, MPI_LONG, recvtask, isnap*NProcs*NProcs+ThisTask+4*NProcs, MPI_COMM_WORLD);
                MPI_Send(&tothalotemporalindex[0],totitems, MPI_LONG, recvtask, isnap*NProcs*NProcs+ThisTask+5*NProcs, MPI_COMM_WORLD);
                MPI_Send(&totmerit[0],totitems, MPI_Real_t, recvtask, isnap*NProcs*NProcs+ThisTask+6*NProcs, MPI_COMM_WORLD);
                MPI_Send(&totdeltat[0],totitems, MPI_INT, recvtask, isnap*NProcs*NProcs+ThisTask+7*NProcs, MPI_COMM_WORLD);
                MPI_Send(&totMPITask[0],totitems, MPI_INT, recvtask, isnap*NProcs*NProcs+ThisTask+8*NProcs, MPI_COMM_WORLD);

                delete[] numdescen;
                delete[] noffset;
                delete[] tothaloindex;
                delete[] tothalotemporalindex;
                delete[] totmerit;
                delete[] totdeltat;
                delete[] totMPITask;
            }
        }
        //receiving information
        else if (itask==ThisTask-jtask) {
            sendtask=itask;
            isnap=mpi_endsnap[itask]-i;
            //store the number of items, allocate memory and then receive
            MPI_Recv(&totitems,1, MPI_Int_t, sendtask, isnap*NProcs*NProcs+sendtask+NProcs, MPI_COMM_WORLD,&status);
            if (totitems>0) {

                numdescen=new int[pht[isnap].numhalos];
                noffset=new long unsigned[pht[isnap].numhalos];
                tothaloindex=new long unsigned[totitems];
                tothalotemporalindex=new long unsigned[totitems];
                totmerit=new Double_t[totitems];
                totdeltat=new int[totitems];
                totMPITask=new int[totitems];

                MPI_Recv(&numdescen[0],pht[isnap].numhalos, MPI_INT, sendtask, isnap*NProcs*NProcs+sendtask+2*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&noffset[0],pht[isnap].numhalos, MPI_LONG, sendtask, isnap*NProcs*NProcs+sendtask+3*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&tothaloindex[0],totitems, MPI_LONG, sendtask, isnap*NProcs*NProcs+sendtask+4*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&tothalotemporalindex[0],totitems, MPI_LONG, sendtask, isnap*NProcs*NProcs+sendtask+5*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&totmerit[0],totitems, MPI_Real_t, sendtask, isnap*NProcs*NProcs+sendtask+6*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&totdeltat[0],totitems, MPI_INT, sendtask, isnap*NProcs*NProcs+sendtask+7*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&totMPITask[0],totitems, MPI_INT, sendtask, isnap*NProcs*NProcs+sendtask+8*NProcs, MPI_COMM_WORLD,&status);

                //then merge the mpi data together
                for (Int_t j=0;j<pht[isnap].numhalos;j++)
                    pprogendescen[isnap][j].Merge(ThisTask,numdescen[j],&tothaloindex[noffset[j]],&tothalotemporalindex[noffset[j]],&totmerit[noffset[j]],&totdeltat[noffset[j]],&totMPITask[noffset[j]]);

                delete[] numdescen;
                delete[] noffset;
                delete[] tothaloindex;
                delete[] tothalotemporalindex;
                delete[] totmerit;
                delete[] totdeltat;
                delete[] totMPITask;
            }
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    //now repeat but going the other way
    for (int itask=NProcs-1;itask>0;itask--) {
    for (int jtask=1;jtask<=mpi_nsenddown[itask];jtask++) {
    //for (int i=0;i<opt.numsteps-1;i++) {
    for (int i=0;i<mpi_senddownnumstep[itask*NProcs+itask-jtask];i++) {
        //sending information
        if (itask==ThisTask) {
            recvtask=ThisTask-jtask;
            isnap=StartSnap+i;
            //build data structures to be sent
            totitems=0;
            for (Int_t j=0;j<pht[isnap].numhalos;j++) {
                totitems+=pprogendescen[isnap][j].NumberofDescendants;
            }
            MPI_Send(&totitems,1, MPI_Int_t, recvtask, isnap*NProcs*NProcs+ThisTask+NProcs, MPI_COMM_WORLD);
            if (totitems>0) {
                numdescen=new int[pht[isnap].numhalos];
                noffset=new long unsigned[pht[isnap].numhalos];
                tothaloindex=new long unsigned[totitems];
                tothalotemporalindex=new long unsigned[totitems];
                totmerit=new Double_t[totitems];
                totdeltat=new int[totitems];
                totMPITask=new int[totitems];

                totitems=0;
                for (Int_t j=0;j<pht[isnap].numhalos;j++) {
                    numdescen[j]=pprogendescen[isnap][j].NumberofDescendants;
                    noffset[j]=totitems;
                    for (Int_t k=0;k<numdescen[j];k++) {
                        tothaloindex[noffset[j]+k]=pprogendescen[isnap][j].haloindex[k];
                        tothalotemporalindex[noffset[j]+k]=pprogendescen[isnap][j].halotemporalindex[k];
                        totmerit[noffset[j]+k]=pprogendescen[isnap][j].Merit[k];
                        totdeltat[noffset[j]+k]=pprogendescen[isnap][j].deltat[k];
                        totMPITask[noffset[j]+k]=pprogendescen[isnap][j].MPITask[k];
                    }
                    totitems+=pprogendescen[isnap][j].NumberofDescendants;
                }

                //then send number of halos, total number of items and then the offset data so that the combined data can be parsed
                MPI_Send(&numdescen[0],pht[isnap].numhalos, MPI_INT, recvtask, isnap*NProcs*NProcs+ThisTask+2*NProcs, MPI_COMM_WORLD);
                MPI_Send(&noffset[0],pht[isnap].numhalos, MPI_LONG, recvtask, isnap*NProcs*NProcs+ThisTask+3*NProcs, MPI_COMM_WORLD);
                MPI_Send(&tothaloindex[0],totitems, MPI_LONG, recvtask, isnap*NProcs*NProcs+ThisTask+4*NProcs, MPI_COMM_WORLD);
                MPI_Send(&tothalotemporalindex[0],totitems, MPI_LONG, recvtask, isnap*NProcs*NProcs+ThisTask+5*NProcs, MPI_COMM_WORLD);
                MPI_Send(&totmerit[0],totitems, MPI_Real_t, recvtask, isnap*NProcs*NProcs+ThisTask+6*NProcs, MPI_COMM_WORLD);
                MPI_Send(&totdeltat[0],totitems, MPI_INT, recvtask, isnap*NProcs*NProcs+ThisTask+7*NProcs, MPI_COMM_WORLD);
                MPI_Send(&totMPITask[0],totitems, MPI_INT, recvtask, isnap*NProcs*NProcs+ThisTask+8*NProcs, MPI_COMM_WORLD);

                delete[] numdescen;
                delete[] noffset;
                delete[] tothaloindex;
                delete[] tothalotemporalindex;
                delete[] totmerit;
                delete[] totdeltat;
                delete[] totMPITask;
            }
        }
        //receiving information
        else if (itask==ThisTask+jtask) {
            sendtask=itask;
            isnap=mpi_startsnap[itask]+i;
            MPI_Recv(&totitems,1, MPI_Int_t, sendtask, isnap*NProcs*NProcs+sendtask+NProcs, MPI_COMM_WORLD,&status);
            if (totitems>0) {

                numdescen=new int[pht[isnap].numhalos];
                noffset=new long unsigned[pht[isnap].numhalos];
                tothaloindex=new long unsigned[totitems];
                tothalotemporalindex=new long unsigned[totitems];
                totmerit=new Double_t[totitems];
                totdeltat=new int[totitems];
                totMPITask=new int[totitems];

                MPI_Recv(&numdescen[0],pht[isnap].numhalos, MPI_INT, sendtask, isnap*NProcs*NProcs+sendtask+2*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&noffset[0],pht[isnap].numhalos, MPI_LONG, sendtask, isnap*NProcs*NProcs+sendtask+3*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&tothaloindex[0],totitems, MPI_LONG, sendtask, isnap*NProcs*NProcs+sendtask+4*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&tothalotemporalindex[0],totitems, MPI_LONG, sendtask, isnap*NProcs*NProcs+sendtask+5*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&totmerit[0],totitems, MPI_Real_t, sendtask, isnap*NProcs*NProcs+sendtask+6*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&totdeltat[0],totitems, MPI_INT, sendtask, isnap*NProcs*NProcs+sendtask+7*NProcs, MPI_COMM_WORLD,&status);
                MPI_Recv(&totMPITask[0],totitems, MPI_INT, sendtask, isnap*NProcs*NProcs+sendtask+8*NProcs, MPI_COMM_WORLD,&status);

                //then merge the mpi data together
                for (Int_t j=0;j<pht[isnap].numhalos;j++)
                    pprogendescen[isnap][j].Merge(ThisTask,numdescen[j],&tothaloindex[noffset[j]],&tothalotemporalindex[noffset[j]],&totmerit[noffset[j]],&totdeltat[noffset[j]],&totMPITask[noffset[j]]);

                delete[] numdescen;
                delete[] noffset;
                delete[] tothaloindex;
                delete[] tothalotemporalindex;
                delete[] totmerit;
                delete[] totdeltat;
                delete[] totMPITask;
            }
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    }
    }

    if (opt.iverbose>0 && ThisTask==0) cout<<"Done"<<endl;
}

#endif