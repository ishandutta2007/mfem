#include "admfem.hpp"
#include "mfem.hpp"

template<typename TDataType, typename TParamVector, typename TStateVector
         , int state_size, int param_size>
class DiffusionFunctional
{
public:
    TDataType operator() (TParamVector& vparam, TStateVector& uu)
    {
        MFEM_ASSERT(state_size==4,"ExampleFunctor state_size should be equal to 4!");
        MFEM_ASSERT(param_size==2,"ExampleFunctor param_size should be equal to 2!");
        auto kapa = vparam[0]; //diffusion coefficient
        auto load = vparam[1]; //volumetric influx
        TDataType rez = kapa*(uu[0]*uu[0]+uu[1]*uu[1]+uu[2]*uu[2])/2.0 - load*uu[3];
        return rez;
    }

};

template<typename TDataType, typename TParamVector, typename TStateVector,
         int residual_size, int state_size, int param_size>
class DiffusionResidual
{
public:
    void operator ()(TParamVector& vparam, TStateVector& uu, TStateVector& rr)
    {
        MFEM_ASSERT(residual_size==4,"DiffusionResidual residual_size should be equal to 4!")
        MFEM_ASSERT(state_size==4,"ExampleFunctor state_size should be equal to 4!");
        MFEM_ASSERT(param_size==2,"ExampleFunctor param_size should be equal to 2!");
        auto kapa = vparam[0]; //diffusion coefficient
        auto load = vparam[1]; //volumetric influx

        rr[0] = kapa * uu[0];
        rr[1] = kapa * uu[1];
        rr[2] = kapa * uu[2];
        rr[3] = -load;
    }
};

template<typename TDataType, typename TParamVector, typename TStateVector,
         int residual_size, int state_size, int param_size>
class ExampleResidual
{
public:
    void operator ()(TParamVector& vparam, TStateVector& uu, TStateVector& rr)
    {
        rr[0]=sin(uu[0]+uu[1]+uu[2]);
        rr[1]=cos(uu[1]+uu[2]+uu[3]);
        rr[2]=tan(uu[2]+uu[3]+uu[4]+uu[5]);
    }

};


int main(int argc, char *argv[])
{

#ifdef MFEM_USE_ADFORWARD
    std::cout<<"MFEM_USE_ADFORWARD == true"<<std::endl;
#else
    std::cout<<"MFEM_USE_ADFORWARD == false"<<std::endl;
#endif

#ifdef MFEM_USE_CALIPER
    cali::ConfigManager mgr;
#endif
    // Caliper instrumentation
    MFEM_PERF_FUNCTION;
    const char* cali_config = "runtime-report";
#ifdef MFEM_USE_CALIPER
    mgr.add(cali_config);
    mgr.start();
#endif
    mfem::Vector param(2);
    param[0]=3.0; //diffusion coefficient
    param[1]=2.0; //volumetric influx

    mfem::Vector state(4);
    state[0]=1.0; // grad_x
    state[1]=2.0; // grad_y
    state[2]=3.0; // grad_z
    state[3]=4.0; // state value

    mfem::QFunctionAutoDiff<DiffusionFunctional,4,2> adf;
    mfem::QVectorFuncAutoDiff<DiffusionResidual,4,4,2> rdf;

    mfem::Vector rr0(4);
    mfem::DenseMatrix hh0(4,4);

    mfem::Vector rr1(4);
    mfem::DenseMatrix hh1(4,4);
MFEM_PERF_BEGIN("QGrad");
    adf.QGrad(param,state,rr0);
MFEM_PERF_END("QGrad");
MFEM_PERF_BEGIN("QHessian");
    adf.QHessian(param, state, hh0);
MFEM_PERF_END("QHessian");
    // dump out the results
    std::cout<<"FunctionAutoDiff"<<std::endl;
    std::cout<< adf.QEval(param,state)<<std::endl;
    rr0.Print(std::cout);
    hh0.Print(std::cout);

MFEM_PERF_BEGIN("QJacobian");
    rdf.QJacobian(param, state, hh1);
MFEM_PERF_END("QJacobian");

    std::cout<<"ResidualAutoDiff"<<std::endl;
    hh1.Print(std::cout);

    //using lambda expression
    auto func = [](mfem::Vector& vparam, mfem::ad::ADVectorType& uu, mfem::ad::ADVectorType& vres) {
    //auto func = [](auto& vparam, auto& uu, auto& vres) { //c++14
        auto kappa = vparam[0]; //diffusion coefficient
        auto load = vparam[1]; //volumetric influx

        vres[0] = kappa * uu[0];
        vres[1] = kappa * uu[1];
        vres[2] = kappa * uu[2];
        vres[3] = -load;
    };

    mfem::VectorFuncAutoDiff<4,4,2> fdr(func);
MFEM_PERF_BEGIN("QJacobianV");
    fdr.QJacobian(param,state,hh1); //computes the gradient of func and stores the result in hh1
MFEM_PERF_END("QJacobianV");
    std::cout<<"LambdaAutoDiff"<<std::endl;
    hh1.Print(std::cout);


    double kappa = param[0];
    double load =  param[1];
    //using lambda expression
    auto func01 = [&kappa,&load](mfem::Vector& vparam, mfem::ad::ADVectorType& uu, mfem::ad::ADVectorType& vres) {
    //auto func = [](auto& vparam, auto& uu, auto& vres) { //c++14

        vres[0] = kappa * uu[0];
        vres[1] = kappa * uu[1];
        vres[2] = kappa * uu[2];
        vres[3] = -load;
    };

    mfem::VectorFuncAutoDiff<4,4,2> fdr01(func01);
MFEM_PERF_BEGIN("QJacobian1");
    fdr01.QJacobian(param,state,hh1);
MFEM_PERF_END("QJacobian1");
    std::cout<<"LambdaAutoDiff 01"<<std::endl;
    hh1.Print(std::cout);


    {
        mfem::QVectorFuncAutoDiff<ExampleResidual,3,6,0> erdf;
        mfem::DenseMatrix vhh(3,6);
        mfem::Vector uu(6);
        mfem::Vector pu;
        uu=1.0;
        erdf.QJacobian(pu,uu,vhh);
        std::cout<<"Last example"<<std::endl;
        vhh.Print(std::cout);

    }


#ifdef MFEM_USE_CALIPER
    mgr.flush();
#endif
}
