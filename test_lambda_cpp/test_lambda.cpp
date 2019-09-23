
/*
Copyright 2010-2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.

This file is licensed under the Apache License, Version 2.0 (the "License").
You may not use this file except in compliance with the License. A copy of
the License is located at

http://aws.amazon.com/apache2.0/

This file is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.
*/
#include <aws/core/Aws.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/logging/DefaultLogSystem.h>
#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/lambda/LambdaClient.h>
#include <aws/lambda/model/CreateFunctionRequest.h>
#include <aws/lambda/model/DeleteFunctionRequest.h>
#include <aws/lambda/model/InvokeRequest.h>
#include <aws/lambda/model/ListFunctionsRequest.h>
#include <aws/states/SFNClient.h>
#include <aws/states/model/ListStateMachinesRequest.h>
#include <aws/states/model/ListStateMachinesResult.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iterator>
#include <libff/common/utils.hpp>
#include <libff/common/serialization.hpp>
#include <libff/algebra/fields/field_utils.hpp>
#include <libff/algebra/fields/bigint.hpp>
#include <libff/algebra/curves/bn128/bn128_pp.hpp>
#include <libff/algebra/fields/fp.hpp>
#include <libff/common/utils.hpp>
#include <libff/common/rng.hpp>


using namespace libff;
using namespace Aws::Lambda;

namespace libff {

   template <typename GroupT>
using run_result_t = std::pair<long long, std::vector<GroupT> >;

template <typename T>
using test_instances_t = std::vector<std::vector<T> >;

template<typename GroupT>
test_instances_t<GroupT> generate_group_elements(size_t count, size_t size)
{
    // generating a random group element is expensive,
    // so for now we only generate a single one and repeat it
    test_instances_t<GroupT> result(count);

    for (size_t i = 0; i < count; i++) {
        GroupT x = GroupT::random_element();
        x.to_special(); // djb requires input to be in special form
        for (size_t j = 0; j < size; j++) {
            result[i].push_back(x);
            // result[i].push_back(GroupT::random_element());
        }
    }

    return result;
}

template<typename FieldT>
test_instances_t<FieldT> generate_scalars(size_t count, size_t size)
{
    // we use SHA512_rng because it is much faster than
    // FieldT::random_element()
    test_instances_t<FieldT> result(count);

    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < size; j++) {
            result[i].push_back(SHA512_rng<FieldT>(i * size + j));
        }
    }

    return result;
}

} // end libff

static const char* ALLOCATION_TAG = "helloLambdaWorld";

static std::shared_ptr<Aws::Lambda::LambdaClient> m_client;
static std::shared_ptr<Aws::SFN::SFNClient> m_statesClient;

static void CreateFunction(Aws::String functionName, Aws::String handler, 
    Aws::Lambda::Model::Runtime runtime, Aws::String roleARN, Aws::String zipFile)
{
    Aws::Lambda::Model::CreateFunctionRequest createFunctionRequest;
    createFunctionRequest.SetHandler(handler);
    createFunctionRequest.SetFunctionName(functionName);
    createFunctionRequest.SetRole(Aws::String(roleARN));
    Aws::Lambda::Model::FunctionCode functionCode;

    std::ifstream fc(zipFile.c_str(), std::ios_base::in | std::ios_base::binary);
    Aws::StringStream buffer;
    buffer << fc.rdbuf();

    functionCode.SetZipFile(Aws::Utils::ByteBuffer((unsigned char*)buffer.str().c_str(), 
                                                   buffer.str().length()));
    createFunctionRequest.SetCode(functionCode);
    createFunctionRequest.SetRuntime(runtime);

    bool done = false;
    while (!done)
    {
        auto outcome = m_client->CreateFunction(createFunctionRequest);
        if (outcome.IsSuccess())
            done = true;
        else
        {
            // Handles case where ROLE is not yet ready
            if (outcome.GetError().GetMessage().find("assume") != std::string::npos)
                std::this_thread::sleep_for(std::chrono::seconds(2));
            else
            {
                done = true;
                std::cout << "\nCreateFunction error:\n"
                    << outcome.GetError().GetMessage() << "\n\n";
            }
        }
    }
}


void DeleteFunction(Aws::String functionName)
{
    Aws::Lambda::Model::DeleteFunctionRequest deleteFunctionRequest;
    deleteFunctionRequest.SetFunctionName(functionName);
    auto outcome = m_client->DeleteFunction(deleteFunctionRequest);
    if (!outcome.IsSuccess())
        std::cout << "\nDeleteFunction error:\n"
        << outcome.GetError().GetMessage() << "\n\n";
}

// Deserialize a string into a vector
//
template<typename T>
std::vector<T> deserializeToVec(std::string s_vec) {
	std::istringstream is(s_vec.c_str());
        return std::vector<T>{ std::istream_iterator<T>( is ), 
                        std::istream_iterator<T>() };
}

// Deserialize a string into a single element
//
template<typename T>
T deserialize(std::string s_elem) {
	T ret;
	std::istringstream is(s_elem.c_str());
    is >> ret;
	return ret;
}

// Serialize a vector into a string
//
template<typename T> 
std::string serializeVec(std::vector<T> vec) {
	std::ostringstream oss;
	std::string str = "";
	std::copy(vec.begin(), vec.end()-1,
                std::ostream_iterator<T>(oss, " "));

    // Now add the last element with no delimiter
    oss << vec.back();
	return oss.str();
}

template<typename T>
std::string serializeFieldVec(std::vector<T> vec) {
    std::ostringstream oss;
    libff::bit_vector bv = libff::convert_field_element_vector_to_bit_vector<T>(vec);
	libff::serialize_bit_vector(oss, bv);
    return oss.str();
}

template<typename T>
std::string serializeField(T field) {
    std::ostringstream oss;
    libff::bit_vector bv = libff::convert_field_element_to_bit_vector<T>(field);
	libff::serialize_bit_vector(oss, bv);
    return oss.str();
}

template<typename T>
std::vector<T> deserializeFieldVec(std::string s_vec) {
    std::istringstream is(s_vec.c_str());
    bit_vector bv;
    libff::deserialize_bit_vector(is, bv);
    return libff::convert_bit_vector_to_field_element_vector<T>(bv);
}

template<typename T>
T deserializeField(std::string s_field) {
    std::istringstream is(s_field.c_str());
    bit_vector bv;
    libff::deserialize_bit_vector(is, bv);
    return libff::convert_bit_vector_to_field_element<T>((const bit_vector)bv);
}

// Serialize a single element into a string
//
template<typename T> 
std::string serialize(T elem) {
	std::ostringstream oss;
	std::string str = "";
    oss << elem;
	return oss.str();
}

void InvokeFunction(Aws::String functionName)
{
    Aws::Lambda::Model::InvokeRequest invokeRequest;
    invokeRequest.SetFunctionName(functionName);
    invokeRequest.SetInvocationType(Aws::Lambda::Model::InvocationType::RequestResponse);
    invokeRequest.SetLogType(Aws::Lambda::Model::LogType::Tail);
    std::shared_ptr<Aws::IOStream> payload = Aws::MakeShared<Aws::StringStream>("FunctionTest");
     
    
    libff::bn128_pp::init_public_params();
    size_t expn = 2;

    test_instances_t<G1<libff::bn128_pp>> group_elements =
            libff::generate_group_elements<G1<libff::bn128_pp>>(10, 1 << expn);
    test_instances_t<Fr<libff::bn128_pp>> scalars =
            libff::generate_scalars<Fr<libff::bn128_pp>>(10, 1 << expn);


    Aws::Utils::Json::JsonValue jsonPayload;
    std::ostringstream oss;
    //std::ostream_iterator<G1<bn128_pp>>(oss, " ");
    std::string str = "";
    std::vector<G1<bn128_pp>> answers;
    printf("size of group elements: %d\n", group_elements.size());
    for (size_t i = 0; i < group_elements.size(); i++) 
    {
        if (!group_elements[i].empty())
        {
                // Convert all but the last element to avoid a trailing ","
                std::copy(group_elements[i].begin(), group_elements[i].end()-1,
                std::ostream_iterator<G1<bn128_pp>>(oss, " "));

                // Now add the last element with no delimiter
                oss << group_elements[i].back();
                if (i==0) {
                for (int k=0;k<group_elements[i].size();k++)
                    printf("%lld ",group_elements[i].at(k));
                std::cout << "\n";
                str = oss.str();
                Aws::String e_str = Aws::Utils::StringUtils::URLEncode(str.c_str());
                jsonPayload.WithString("groupelements", e_str);//oss.str());
                std::cout << "serialized: " << str.c_str() << "\n";
            }
	    }
    }
    *payload << jsonPayload.View().WriteReadable();

    invokeRequest.SetBody(payload);
    invokeRequest.SetContentType("application/text");
    auto outcome = m_client->Invoke(invokeRequest);
    auto &result = outcome.GetResult();

    printf("Outcome is success %d \n", outcome.IsSuccess());
    if (outcome.IsSuccess())
    {
        //auto &result = outcome.GetResult();

        // Lambda function result (key1 value)
        Aws::IOStream& payload = result.GetPayload();
        Aws::String functionResult;
        std::getline(payload, functionResult);
        std::cout << "Lambda result:\n" << functionResult << "\n\n";

        /*// Decode the result header to see requested log information 
        auto byteLogResult = Aws::Utils::HashingUtils::Base64Decode(result.GetLogResult());
        Aws::StringStream logResult;
        for (unsigned i = 0; i < byteLogResult.GetLength(); i++)
            logResult << byteLogResult.GetItem(i);
        std::cout << "Log result header:\n" << logResult.str() << "\n\n";
        */
    }
}

void lambdaCallback(const LambdaClient* client, 
                    const Model::InvokeRequest& req, 
                    const Model::InvokeOutcome& outcome, 
                    const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context)
{
    //auto const& result = outcome.GetResult();

    //printf("Outcome is success %d \n", outcome.IsSuccess());
    //if (outcome.IsSuccess())
    //{
        printf("hi\n");
        /*
        // Lambda function result (key1 value)
        const Aws::IOStream& payload = result.GetPayload();
        Aws::String functionResult;
        std::getline(payload, functionResult);
        //std::cout << "Lambda result:\n" << functionResult << "\n\n";
        //return functionResult;
        auto answer = deserialize<G1<libff::bn128_pp>>(functionResult.c_str());
        printf("%lld ", answer);
        */
    //}
}

Aws::String InvokeFunctionAsync(Aws::String functionName, Aws::Utils::Json::JsonValue jsonBody)
{
    Aws::Lambda::Model::InvokeRequest invokeRequest;
    invokeRequest.SetFunctionName(functionName);
    invokeRequest.SetInvocationType(Aws::Lambda::Model::InvocationType::Event);//::RequestResponse);
    //invokeRequest.SetLogType(Aws::Lambda::Model::LogType::Tail);
    std::shared_ptr<Aws::IOStream> payload = Aws::MakeShared<Aws::StringStream>("");

    *payload << jsonBody.View().WriteReadable();

    //invokeRequest.SetBody(payload);
    //invokeRequest.SetContentType("application/text");
    printf("invoking\n");
    m_client->InvokeAsync(invokeRequest, lambdaCallback);
    //auto outcome = m_client->Invoke(invokeRequest);
    printf("done\n");
    
    /*
    auto &result = outcome.GetResult();

    printf("Outcome is success %d \n", outcome.IsSuccess());
    if (outcome.IsSuccess())
    {
       // Lambda function result (key1 value)
       Aws::IOStream& payload = result.GetPayload();
       Aws::String functionResult;
       std::getline(payload, functionResult);
        std::cout << "Lambda result:\n" << functionResult << "\n\n";
       return functionResult;
        // Decode the result header to see requested log information 
        auto byteLogResult = Aws::Utils::HashingUtils::Base64Decode(result.GetLogResult());
        Aws::StringStream logResult;
        for (unsigned i = 0; i < byteLogResult.GetLength(); i++)
            logResult << byteLogResult.GetItem(i);
        std::cout << "Log result header:\n" << logResult.str() << "\n\n";
        
    }*/
    return "";
}

Aws::String InvokeFunction(Aws::String functionName, Aws::Utils::Json::JsonValue jsonBody)
{
    Aws::Lambda::Model::InvokeRequest invokeRequest;
    invokeRequest.SetFunctionName(functionName);
    invokeRequest.SetInvocationType(Aws::Lambda::Model::InvocationType::RequestResponse);
    invokeRequest.SetLogType(Aws::Lambda::Model::LogType::Tail);
    std::shared_ptr<Aws::IOStream> payload = Aws::MakeShared<Aws::StringStream>("");

    *payload << jsonBody.View().WriteReadable();

    invokeRequest.SetBody(payload);
    invokeRequest.SetContentType("application/text");
    //printf("invoking\n");
    auto outcome = m_client->Invoke(invokeRequest);
    //m_client->InvokeAsync(invokeRequest, lambdaCallback);
    //printf("done\n");
    auto &result = outcome.GetResult();

    printf("Outcome is success %d with ret code: %d \n", 
                outcome.IsSuccess(),
                outcome.GetResult().GetStatusCode());
    if (outcome.IsSuccess())
    {
       // Lambda function result (key1 value)
       Aws::IOStream& payload = result.GetPayload();
       Aws::String functionResult;
       std::getline(payload, functionResult);
       //std::cout << "Lambda result:\n" << functionResult << "\n\n";
       return functionResult;
        /*// Decode the result header to see requested log information 
        auto byteLogResult = Aws::Utils::HashingUtils::Base64Decode(result.GetLogResult());
        Aws::StringStream logResult;
        for (unsigned i = 0; i < byteLogResult.GetLength(); i++)
            logResult << byteLogResult.GetItem(i);
        std::cout << "Log result header:\n" << logResult.str() << "\n\n";
        */
    }
    return "";
}

void InvokeMultiExpInner2()
{   
    libff::bn128_pp::init_public_params();
    size_t expn = 2;

    test_instances_t<G1<libff::bn128_pp>> group_elements =
            libff::generate_group_elements<G1<libff::bn128_pp>>(3, 1 << expn);
    test_instances_t<Fr<libff::bn128_pp>> scalars =
            libff::generate_scalars<Fr<libff::bn128_pp>>(3, 1 << expn);


    Aws::Utils::Json::JsonValue jsonPayload;
    std::vector<G1<bn128_pp>> answers;
    printf("size of group elements: %d\n", group_elements.size());
    
    for (size_t i = 0; i < group_elements.size(); i++) 
    {
        size_t chunks = group_elements[i].size() / 2;
        // Chunker
        std::vector<G1<libff::bn128_pp>>::const_iterator vec_start = group_elements[i].cbegin();
        std::vector<G1<libff::bn128_pp>>::const_iterator vec_end = group_elements[i].cend();
        std::vector<Fr<libff::bn128_pp>>::const_iterator scalar_start = scalars[i].cbegin();
        std::vector<Fr<libff::bn128_pp>>::const_iterator scalar_end = scalars[i].cend();
        const size_t total = vec_end - vec_start;

        const size_t one = total/chunks;

        std::vector<G1<libff::bn128_pp>> partial(chunks, G1<libff::bn128_pp>::zero());

        for (size_t j = 0; j < chunks; ++j)
        {
            printf("%d\n", j);
            std::vector<G1<libff::bn128_pp>> ge{vec_start + j*one,
                (j == chunks-1 ? vec_end : vec_start + (j+1)*one)};
            std::vector<Fr<libff::bn128_pp>> sc{scalar_start + j*one,
                (j == chunks-1 ? scalar_end : scalar_start + (j+1)*one)};
            std::string ge_ser = serializeVec<G1<libff::bn128_pp>>(ge);
            Aws::String ge_str = Aws::Utils::StringUtils::URLEncode(ge_ser.c_str());
            printf("%s\n%s\n", ge_ser.c_str(), ge_str.c_str());
            jsonPayload.WithString("groupelements", ge_str);

            std::string sc_ser = serializeFieldVec<Fr<libff::bn128_pp>>(sc);
            Aws::String sc_str = Aws::Utils::StringUtils::URLEncode(sc_ser.c_str());
            jsonPayload.WithString("scalars", sc_str);
            Aws::String str("multiexp");
            /*Aws::String answer = */InvokeFunction(str, jsonPayload);
            //partial[j] = deserialize<G1<libff::bn128_pp>>(answer.c_str());
        }

        //G1<libff::bn128_pp> final = G1<libff::bn128_pp>::zero();

        //for (size_t j = 0; j < chunks; ++j)
        //{
        //    final = final + partial[j];
        //}
        
        //answers.push_back(final);
    }

    //Output
    // for(int i=0; i<answers.size(); i++)
    //    printf("%lld ", answers[i]);
    // printf("\n");
}

void InvokeMultiExpInner()
{   
    libff::bn128_pp::init_public_params();
    size_t expn = 2;

    test_instances_t<G1<libff::bn128_pp>> group_elements =
            libff::generate_group_elements<G1<libff::bn128_pp>>(10, 1 << expn);
    test_instances_t<Fr<libff::bn128_pp>> scalars =
            libff::generate_scalars<Fr<libff::bn128_pp>>(10, 1 << expn);


    Aws::Utils::Json::JsonValue jsonPayload;
    std::vector<G1<bn128_pp>> answers;
    printf("size of group elements: %d\n", group_elements.size());
    for (size_t i = 0; i < group_elements.size(); i++) 
    {
        std::string ge_ser = serializeVec<G1<libff::bn128_pp>>(group_elements[i]);
        Aws::String ge_str = Aws::Utils::StringUtils::URLEncode(ge_ser.c_str());
        jsonPayload.WithString("groupelements", ge_str);

        std::string sc_ser = serializeFieldVec<Fr<libff::bn128_pp>>(scalars[i]);
        Aws::String sc_str = Aws::Utils::StringUtils::URLEncode(sc_ser.c_str());
        jsonPayload.WithString("scalars", sc_str);
        
        Aws::String answer = InvokeFunction("multiexp", jsonPayload);
        answers.push_back(deserialize<G1<libff::bn128_pp>>(answer.c_str()));
    }

    //Output
    for(int i=0; i<answers.size(); i++)
       printf("%lld ", answers[i]);
    printf("\n");
}

void ListFunctions()
{
    Aws::Lambda::Model::ListFunctionsRequest listFunctionsRequest;
    auto listFunctionsOutcome = m_client->ListFunctions(listFunctionsRequest);
    auto functions = listFunctionsOutcome.GetResult().GetFunctions();
    std::cout << functions.size() << " function(s):" << std::endl;
    for(const auto& item : functions)
        std::cout << item.GetFunctionName() << std::endl;
    std::cout << std::endl;
}

void InvokeSfn()
{
    // Model::ListStateMachinesOutcome ListStateMachines(const Model::ListStateMachinesRequest& request)
    Aws::SFN::Model::ListStateMachinesRequest mcrequest;
    auto outcome = m_statesClient->ListStateMachines(mcrequest);
    auto machines = outcome.GetResult().GetStateMachines();
    std::cout << machines.size() << " machine(s):" << std::endl;
    for(const auto& item : machines)
        std::cout << item.GetName() << std::endl;
    std::cout << std::endl;
}

int main(int argc, char **argv)
{
    // Configuration Properties|Debug, Environment=AWS_DEFAULT_PROFILE=default$(LocalDebuggerEnvironment)
    const Aws::String USAGE = "\n" \
        "Description\n"
        "     This sample creates a function from a zip file, lists available functions,\n"
        "     invokes the newly created function, and then deletes the function.\n"
        "     The function should take three arguments and return a string, see \n\n"
        "     http://docs.aws.amazon.com/lambda/latest/dg/get-started-create-function.html.\n\n"
        "Usage:\n"
        "     lambda_example name handler runtime rolearn zipfile <region>\n\n"
        "Where:\n"
        "    name   - lambda function name to create\n"
        "    handler- function name in code to call\n"
        "    runtime- runtime to use for function:\n"
        "             nodejs,nodejs4.3,java8,python2.7,dotnetcore1.0,nodejs4.3.edge\n"
        "    rolearn- rule lambda will assume when running function\n"
        "    zipfile- zip file containing function and other dependencies\n"
        "    region - optional region, e.g. us-east-2\n\n"
        "Example:\n"
        "    create_function helloLambdaWorld helloLambdaWorld.handler python2_7 ***arn*** helloLambdaWorld.zip\n\n";

    if (argc < 5)
    {
        std::cout << USAGE;
        //return 1;
    }
    // Enable logging to help diagnose service issues
    const bool logging = false;

    Aws::SDKOptions options;
    Aws::InitAPI(options);
    {
        if (logging)
            Aws::Utils::Logging::InitializeAWSLogging(
                Aws::MakeShared<Aws::Utils::Logging::DefaultLogSystem>(
                    "create_function",
                    Aws::Utils::Logging::LogLevel::Trace,
                    "create_function_"));

        const Aws::String functionName("multiexp");//argv[1]);

        //const Aws::String functionHandler(argv[2]);
        //const Aws::Lambda::Model::Runtime functionRuntime = 
        //    Aws::Lambda::Model::RuntimeMapper::GetRuntimeForName(Aws::String(argv[3]));
        //const Aws::String functionRoleARN(argv[4]);
        //const Aws::String functionZipFile(argv[5]);
        //const Aws::String region(argc > 5 ? argv[6] : "");

        Aws::String region("");
        Aws::Client::ClientConfiguration clientConfig;
        if (!region.empty())
            clientConfig.region = region;
        m_client = Aws::MakeShared<Aws::Lambda::LambdaClient>(ALLOCATION_TAG, 
                                                              clientConfig);
        m_statesClient = Aws::MakeShared<Aws::SFN::SFNClient>(ALLOCATION_TAG, 
                                                              clientConfig);                                                    

        //CreateFunction(functionName, functionHandler, functionRuntime,
                       //functionRoleARN, functionZipFile);

        //ListFunctions();

        //InvokeFunction(functionName);
	    //InvokeFunction(functionName);
	    //InvokeFunction(functionName);
        //InvokeMultiExpInner();
        InvokeSfn();
        //DeleteFunction(functionName);

        m_client = nullptr;

        if(logging)
            Aws::Utils::Logging::ShutdownAWSLogging();
    }
    Aws::ShutdownAPI(options);

    return 0;
}
