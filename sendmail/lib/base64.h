#ifndef BASE64_H_
#define BASE64_H_

#ifdef __cplusplus
extern "C"
{
#endif

int base64_encode(const unsigned char* input,int input_length,unsigned char* output,int output_length);

int base64_decode(const unsigned char* input,int input_length,unsigned char* output,int output_length);

#ifdef __cplusplus
}
#endif

#endif /* BASE64_H_ */
