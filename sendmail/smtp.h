
#ifndef SMTP_H_
#define SMTP_H_



#ifdef __cplusplus
extern "C"
{
#endif


int smtp_send(const char* domain,int port,const char* user_name,const char* password,
              const char* subject,const char* content,const char** to,int to_len);


#ifdef __cplusplus
}
#endif /* end of extern "C" */

#endif /* SMTP_H_ */
