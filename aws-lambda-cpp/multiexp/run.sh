rm multiexp.zip
rm multiexp 
cmake ..
make 
make aws-lambda-package-multiexp
aws lambda update-function-code --function-name multiexp --zip-file fileb://multiexp.zip

