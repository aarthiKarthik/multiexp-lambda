// main.cpp
#include <algorithm>
#include <cassert>
#include <type_traits>
#include <aws/core/Aws.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/core/utils/logging/ConsoleLogSystem.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/HashingUtils.h>

#include <libff/algebra/curves/bn128/bn128_pp.hpp>
#include <libff/algebra/fields/bigint.hpp>
#include <libff/algebra/fields/fp_aux.tcc>
#include <libff/algebra/scalar_multiplication/multiexp.hpp>
#include <libff/algebra/scalar_multiplication/wnaf.hpp>
//#include <libff/common/profiling.hpp>
#include <libff/common/utils.hpp>
#include <libff/common/rng.hpp>
#include <aws/lambda-runtime/runtime.h>

using namespace libff;
using namespace aws::lambda_runtime;


namespace libff {
template<typename T, typename FieldT, multi_exp_method Method,
    typename std::enable_if<(Method == multi_exp_method_BDLO12), int>::type = 0>
T multi_exp_inner1(
    typename std::vector<T>::const_iterator bases,
    typename std::vector<T>::const_iterator bases_end,
    typename std::vector<FieldT>::const_iterator exponents,
    typename std::vector<FieldT>::const_iterator exponents_end)
    {
    	UNUSED(exponents_end);
    	size_t length = bases_end - bases;

    	// empirically, this seems to be a decent estimate of the optimal value of c
    	size_t log2_length = log2(length);
    	size_t c = log2_length - (log2_length / 3 - 2);

    	const mp_size_t exp_num_limbs =
        	std::remove_reference<decltype(*exponents)>::type::num_limbs;
    	std::vector<bigint<exp_num_limbs> > bn_exponents(length);
    	size_t num_bits = 0;

    	for (size_t i = 0; i < length; i++)
    	{
        	bn_exponents[i] = exponents[i].as_bigint();
        	num_bits = std::max(num_bits, bn_exponents[i].num_bits());
    	}

    	size_t num_groups = (num_bits + c - 1) / c;

    	T result;
    	bool result_nonzero = false;

    	for (size_t k = num_groups - 1; k <= num_groups; k--)
    	{
        	if (result_nonzero)
        	{
            		for (size_t i = 0; i < c; i++)
            		{
                		result = result.dbl();
            		}
        	}

        	std::vector<T> buckets(1 << c);
        	std::vector<bool> bucket_nonzero(1 << c);

        	for (size_t i = 0; i < length; i++)
        	{
            		size_t id = 0;
            		for (size_t j = 0; j < c; j++)
           	 	{
                		if (bn_exponents[i].test_bit(k*c + j))
                		{
                    			id |= 1 << j;
                		}
            		}

            		if (id == 0)
            		{
                		continue;
            		}

            		if (bucket_nonzero[id])
            		{
#ifdef USE_MIXED_ADDITION
                		buckets[id] = buckets[id].mixed_add(bases[i]);
#else
                		buckets[id] = buckets[id] + bases[i];
#endif
            		}
           		else
            		{
                		buckets[id] = bases[i];
                		bucket_nonzero[id] = true;
            		}
        	}

#ifdef USE_MIXED_ADDITION
        	batch_to_special(buckets);
#endif

        	T running_sum;
        	bool running_sum_nonzero = false;

        	for (size_t i = (1u << c) - 1; i > 0; i--)
        	{
            		if (bucket_nonzero[i])
            		{
                		if (running_sum_nonzero)
                		{
#ifdef USE_MIXED_ADDITION
                    			running_sum = running_sum.mixed_add(buckets[i]);
#else
                    			running_sum = running_sum + buckets[i];
#endif
                		}
                		else
                		{
                    			running_sum = buckets[i];
                    			running_sum_nonzero = true;
                		}	
            		}

            		if (running_sum_nonzero)
            		{
                		if (result_nonzero)
                		{
                    			result = result + running_sum;
                		}
                		else
                		{
                    			result = running_sum;
                    			result_nonzero = true;
                		}
            		}
        	}
    	}

	return result;
    	//return invocation_response::success(result, "application/json");
    }


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
}

int multi_exp_run()
{
   libff::bn128_pp::init_public_params();
   size_t expn = 2;

   test_instances_t<G1<libff::bn128_pp>> group_elements =
            libff::generate_group_elements<G1<libff::bn128_pp>>(10, 1 << expn);
   test_instances_t<Fr<libff::bn128_pp>> scalars =
            libff::generate_scalars<Fr<libff::bn128_pp>>(10, 1 << expn);

   std::vector<G1<bn128_pp>> answers;
   for (size_t i = 0; i < group_elements.size(); i++) {
        answers.push_back(libff::multi_exp_inner1<G1<bn128_pp>, Fr<bn128_pp>, multi_exp_method_BDLO12, 1>(group_elements[i].cbegin(), group_elements[i].cend(), scalars[i].cbegin(), scalars[i].cend()));
        printf("\t%lld\n", answers.at(i)); fflush(stdout);
   } 
   printf("\n\n\t%lld\n", answers); fflush(stdout);  
   
   return answers.size();
}



invocation_response my_handler(invocation_request const& request)
{
   using namespace Aws::Utils::Json;
    JsonValue json((const Aws::String&)request.payload);
    if (!json.WasParseSuccessful()) {
        return invocation_response::failure("Failed to parse input JSON", "InvalidJSON");
    }

    auto v = json.View();

   /* if (!v.ValueExists("s3bucket") || !v.ValueExists("s3key") || !v.GetObject("s3bucket").IsString() ||
        !v.GetObject("s3key").IsString()) {
        return invocation_response::failure("Missing input value s3bucket or s3key", "InvalidJSON");
    }*/

    if (!v.ValueExists("key1")) {
        return invocation_response::failure("Failed to parse input JSON", "InvalidJSON");
    }

    auto key1 = v.GetString("key1");
    auto key2 = v.GetString("key2");

    /*AWS_LOGSTREAM_INFO(TAG, "Attempting to download file from s3://" << bucket << "/" << key);

    Aws::String base64_encoded_file;
    auto err = download_and_encode_file(client, bucket, key, base64_encoded_file);
    if (!err.empty()) {
        return invocation_response::failure(err, "DownloadFailure");
    }*/


   int len = multi_exp_run();
   //return invocation_response::success(key1.c_str(), "application/json");

   return invocation_response::success(std::to_string(len).c_str(), "application/json");
}

int main()
{
   run_handler(my_handler);
   //multi_exp_run();
    
   return 0;
}


