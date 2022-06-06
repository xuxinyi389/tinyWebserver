#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

class sem//�ź�����
{
public:
    sem()//���캯��
    {
        if (sem_init(&m_sem, 0, 0) != 0)//�ź�����ʼ��Ϊ0
        {
            throw std::exception(); //�ź�����ʼ��ʧ�� �׳��쳣 ���ѿ���Ȩת�Ƹ��ܹ�������쳣�Ĵ���
        }
    }
    sem(int num)//�вι��캯��
    {
        if (sem_init(&m_sem, 0, num) != 0)//�ź�����ʼ��Ϊnum
        {
            throw std::exception();//�ź�����ʼ��ʧ�� �׳��쳣 ���ѿ���Ȩת�Ƹ��ܹ�������쳣�Ĵ���
        }
    }
    ~sem()//�������� 
    {
        sem_destroy(&m_sem);//�ź�������
    }
    bool wait()
    {
        return sem_wait(&m_sem) == 0;//sem_wait��������ԭ�Ӳ�����ʽ���ź�����һ �ź���Ϊ0ʱ sem_wait���� �ɹ�ʱ����0
    }
    bool post()
    {
        return sem_post(&m_sem) == 0;//sem_post������ԭ�Ӳ�����ʽ���ź�����һ,�ź�������0ʱ,���ѵ���sem_post���߳� �ɹ�ʱ����0
    }

private:
    sem_t m_sem;
};
class locker//��������
{
public:
    locker()//���캯��
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();//������ʧ�� �׳��쳣 ���ѿ���Ȩת�Ƹ��ܹ�������쳣�Ĵ���
        }
    }
    ~locker()//��������
    {
        pthread_mutex_destroy(&m_mutex);//���ٻ�����
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;//��ԭ�Ӳ�����ʽ������������ �ɹ�����0 ʧ�ܷ���errno
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;//��ԭ�Ӳ�����ʽ������������ �ɹ�����0 ʧ�ܷ���errno
    }
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex; //���������� 
};
class cond //���������� ���������ṩ��һ���̼߳��֪ͨ����,��ĳ���������ݴﵽĳ��ֵʱ,���ѵȴ�����������ݵ��߳�
{
public:
    cond()//���캯��
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)//��ʼ����������
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()//����
    {
        pthread_cond_destroy(&m_cond);//������������
    }
    bool wait(pthread_mutex_t *m_mutex) //�����ȴ� ����0��ʾ�ɹ� 
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        //�ú������ڵȴ�Ŀ����������� mutex������Ϊ�����ڱ������������Ļ����� ��ȷ��wait������ԭ����
        //�ڵ���waitǰ ����ȷ������������ wait����ִ��ʱ ���Ȱѵ����̷߳������������ĵȴ������� 
        //Ȼ��mutex���� �ɼ� ��wait��ʼִ�е�������̱߳�����ȴ�����֮���ʱ���� signal��broadcvast�Ⱥ���
        //�����޸��������� ��wait�ɹ�����ʱ ������mutex���ٴα�����
        ret = pthread_cond_wait(&m_cond, m_mutex);//�ú�������ʱ��Ҫ���� mutex����(�����Ļ�����) ,����ִ��ʱ,�Ȱѵ����̷߳��������������������,Ȼ�󽫻�����mutex����,�������ɹ�����Ϊ0ʱ,���������ٴα�����. Ҳ����˵�����ڲ�����һ�ν����ͼ�������
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)//��ʱ�ȴ�  ����0��ʾ�ɹ� 
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);//����ʱ��᷵�� ETIMEDOUT
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()//����һ���ȴ����������̣߳����ڶ���ȴ��߳�ʱ�����˳�򼤻�����һ��
    {
        return pthread_cond_signal(&m_cond) == 0;
    }
    bool broadcast()// �Թ㲥�ķ�ʽ�������еȴ�Ŀ��������߳�
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};
#endif
