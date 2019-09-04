#   Copyright 2010-2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.

#   This file is licensed under the Apache License, Version 2.0 (the "License").
#   You may not use this file except in compliance with the License. A copy of
#   the License is located at

#   http://aws.amazon.com/apache2.0/

#   This file is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
#   CONDITIONS OF ANY KIND, either express or implied. See the License for the
#   specific language governing permissions and limitations under the License.

cmake_minimum_required(VERSION 3.2)
project(test_lambda LANGUAGES CXX)
set (CMAKE_CXX_STANDARD 11)

# Locate the aws sdk for c++ package.
find_package(AWSSDK REQUIRED COMPONENTS lambda libaws-cpp-sdk-core libaws-cpp-sdk-lambda)
find_package(OpenSSL REQUIRED)
add_library(aws-core SHARED IMPORTED)
set_target_properties(aws-core PROPERTIES IMPORTED_LOCATION "/usr/local/lib/libaws-cpp-sdk-core.so")
add_library(aws-lambda SHARED IMPORTED)
set_target_properties(aws-lambda PROPERTIES IMPORTED_LOCATION "/usr/local/lib/libaws-cpp-sdk-lambda.so")

# The executables to build.
set(EXAMPLES "")
list(APPEND EXAMPLES "test_lambda")

# The executables to build
foreach(EXAMPLE IN LISTS EXAMPLES)
  add_executable(${EXAMPLE} ${EXAMPLE}.cpp)
  target_link_libraries(${EXAMPLE} ${AWSSDK_LINK_LIBRARIES})
endforeach()

target_link_libraries(${PROJECT_NAME} aws-core aws-lambda libff.a gmp libzm.a OpenSSL::SSL)

