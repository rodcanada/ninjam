#include <windows.h>

#include "../../WDL/jnetlib/jnetlib.h"
#include "../netmsg.h"
#include "../mpb.h"

#include "../audiostream.h"

#include "../vorbisencdec.h"

extern char *get_asio_configstr();



CRITICAL_SECTION net_cs;
audioStreamer *g_audio;
Net_Connection *netcon=0;

VorbisEncoder *g_vorbisenc;

WDL_Queue vorbisrecvbuf;
VorbisDecoder netdec;

int g_audio_enable;
void audiostream_onunder() { }
void audiostream_onover() { }

WDL_Queue m_outoggbuf;

#define MIN_ENC_BLOCKSIZE 8192
#define MAX_ENC_BLOCKSIZE (16384-128)

void audiostream_onsamples(float *buf, int len, int nch)
{
  if (!g_audio_enable) 
  {
    memset(buf,0,len*nch*sizeof(float));
    return;
  }

  // encode my audio and send to server
  char outbuf[65536];
  int obl=g_vorbisenc->Encode(buf,len,outbuf,sizeof(outbuf));
  if (obl)
  {
    m_outoggbuf.Add(outbuf,obl);
  }

  while (m_outoggbuf.Available()>MIN_ENC_BLOCKSIZE)
  {
    int s=m_outoggbuf.Available();
    if (s > MAX_ENC_BLOCKSIZE) s=MAX_ENC_BLOCKSIZE;

    {
      mpb_client_upload_interval_write wh;
      wh.transfer_id=1;
      wh.audio_data=m_outoggbuf.Get();
      wh.audio_data_len=s;

      EnterCriticalSection(&net_cs);
      //send
      netcon->Send(wh.build());
      LeaveCriticalSection(&net_cs);
    }

    m_outoggbuf.Advance(s);
  }
  m_outoggbuf.Compact();

  EnterCriticalSection(&net_cs);
  while (netdec.m_samples_used < len*nch && vorbisrecvbuf.GetSize()>0)
  {
    int l=vorbisrecvbuf.GetSize();
    if (l > 4096) l=4096;
    netdec.Decode((char*)vorbisrecvbuf.Get(),l);
    vorbisrecvbuf.Advance(l);
  }
  vorbisrecvbuf.Compact();

  LeaveCriticalSection(&net_cs);
  // decode any available incoming streams and mix in

  float *sptr=(float *)netdec.m_samples.Get();
  int sch=netdec.GetNumChannels();
  int mixl=min(len*sch,netdec.m_samples_used);

  int x;
  for (x = 0; x < len; x ++)
  {    
    float vl,vr;
    if (sch == 1)
    {
      vl=vr=sptr[x];
    }
    else
    {
      vl=sptr[x+x];
      vr=sptr[x+x+1];
      if (nch == 1) vl=(vl+vr)*0.5f;
    }

    // process vl/vr here

    if (nch == 1) buf[x]+=vl;
    else
    {
      buf[x+x]+=vl;
      buf[x+x+1]+=vr;
    }
 

  }
  memcpy(sptr,sptr+mixl,(netdec.m_samples_used-mixl)*sizeof(float));
  netdec.m_samples_used -= mixl;
}



int main(int argc, char **argv)
{
  InitializeCriticalSection(&net_cs);
  printf("Ninjam v0.0 command line client starting up...\n");

  if (argc != 2)
  {
    printf("Usage: ninjam hostname\n");
    return 0;
  }

  g_audio_enable=0;

  {
    char *dev_name_in=get_asio_configstr();
    audioStreamer_ASIO *audio=new audioStreamer_ASIO;

    int nbufs=2,bufsize=4096;
    if (audio->Open(&dev_name_in,48000,2,16,-1,&nbufs,&bufsize))
    {
      printf("Error opening audio!\n");
      return 0;
    }
    printf("Opened %s\n",dev_name_in);
    g_audio=audio;
  }

  JNL::open_socketlib();

  printf("Creating audio encoder... (%dhz %dch)",g_audio->m_srate,g_audio->m_nch);
  g_vorbisenc = new VorbisEncoder(g_audio->m_srate,g_audio->m_nch,0.25);
  if (g_vorbisenc->isError()) printf("ERROR\n");
  else printf("YAY\n");

  int m_bpm=120,m_bpi=32;
  {
    printf("Connecting to %s...\n",argv[1]);
    JNL_Connection *con=new JNL_Connection(JNL_CONNECTION_AUTODNS,65536,65536);
    con->connect(argv[1],2049);

    for (;;)
    {
      con->run();
      if (con->get_state() < JNL_Connection::STATE_RESOLVING || con->get_state() >= JNL_Connection::STATE_CONNECTED)
        break;
      Sleep(100);
    }
    if (con->get_state() != JNL_Connection::STATE_CONNECTED)
    {
      printf("Error connecting!\n");
      delete con;
    }
    else
    {
      printf("Connected!\n");

      netcon = (Net_Connection*)new Net_Connection;
      netcon->attach(con);
    }
  }

  if (netcon)
  {

    while (!netcon->GetStatus())
    {
      EnterCriticalSection(&net_cs);
      Net_Message *msg=netcon->Run();
      LeaveCriticalSection(&net_cs);
      if (msg)
      {
        msg->addRef();

        switch (msg->get_type())
        {
          case MESSAGE_SERVER_AUTH_CHALLENGE:
            {

              mpb_server_auth_challenge cha;
              if (!cha.parse(msg))
              {
                printf("Got challenge.\n");

                char buf[512],pass[512];
                printf("Enter username: ");
                fflush(stdout);
                gets(buf);
                printf("Enter password: ");
                fflush(stdout);
                gets(pass);

                mpb_client_auth_user repl;
                repl.username=buf;
                // fucko: make password hash

                EnterCriticalSection(&net_cs);
                netcon->Send(repl.build());
                LeaveCriticalSection(&net_cs);
              }
            }
          break;
          case MESSAGE_SERVER_AUTH_REPLY:
            {
              mpb_server_auth_reply ar;
              if (!ar.parse(msg))
              {
                printf("Got auth reply of %d\n",ar.flag);


                if (ar.flag) // send our channel information
                {
                  mpb_client_set_channel_info sci;

                  sci.build_add_rec("default channel",0,0,0);

                  EnterCriticalSection(&net_cs);
                  netcon->Send(sci.build());
                  LeaveCriticalSection(&net_cs);
                }
                else 
                {
                  EnterCriticalSection(&net_cs);
                  netcon->Kill();
                  LeaveCriticalSection(&net_cs);
                }
              }
            }
          break;
          case MESSAGE_SERVER_CONFIG_CHANGE_NOTIFY:
            {
              mpb_server_config_change_notify ccn;
              if (!ccn.parse(msg))
              {
                m_bpm=ccn.beats_minute;
                m_bpi=ccn.beats_interval;
                printf("Got info update from server, bpm=%d, bpi=%d\n",m_bpm,m_bpi);
                g_audio_enable=1;
              }
            }

          break;
          case MESSAGE_SERVER_USERINFO_CHANGE_NOTIFY:
            {
              mpb_server_userinfo_change_notify ucn;
              if (!ucn.parse(msg))
              {
                int offs=0;
                int a=0, cid=0, p=0,f=0;
                short v=0;
                char *un=0,*chn=0;
                while ((offs=ucn.parse_get_rec(offs,&a,&cid,&v,&p,&f,&un,&chn))>0)
                {
                  if (!un) un="";
                  if (!chn) chn="";
                  printf("user %s, channel %d \"%s\": %s v:%d.%ddB p:%d flag=%d\n",un,cid,chn,a?"active":"inactive",(int)v/10,abs((int)v)%10,p,f);
                }
              }
            }
          break;
          case MESSAGE_SERVER_DOWNLOAD_INTERVAL_WRITE:
            {
              mpb_server_download_interval_write diw;
              diw.parse(msg);
              if (diw.audio_data_len > 0&& diw.audio_data) 
              {
                EnterCriticalSection(&net_cs);
                vorbisrecvbuf.Add(diw.audio_data,diw.audio_data_len);              
                LeaveCriticalSection(&net_cs);
              }
            }
          break;
          default:
            printf("Got unknown message %02X\n",msg->get_type());
          break;
        }

        msg->releaseRef();
      }
      else Sleep(10);
    }
  }


  delete g_audio;

  JNL::close_socketlib();
  return 0;
}
