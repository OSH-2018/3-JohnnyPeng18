#define FUSE_USE_VERSION 26
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse.h>
#include <sys/mman.h>
#include <stdio.h>

static const size_t size = 4 * 1024 * 1024 * (size_t)1024;  //文件系统最大4G	
static char *mem;							//记录block使用情况
static int *data;
static size_t blocknr = 1024 * 1024;					//存储空间共分成512*1024块，每块大小4K
static size_t blocksize = 4 * (size_t)1024;

struct fsmetadata{								//文件系统基本数据
	size_t fs_size;
	size_t fs_blocknr;
	size_t fs_blocksize;
	struct filenode *root;
	struct mem *memusage;                          
}*fs_metadata;

struct filenode{
	char name[128];
	void *content[256];											//文件最多被分为256块，连续的block算作一块
	int usedblock[257][2];										//记录每一个content指针用的block数和block首地址
	struct stat st;
	struct filenode *next;
};

struct mem{
	char mem[1024*1024];			//标记各个block是否被使用
    void *mempoint[1024*1024];           //各个block的指针
	int emptynum;					//空block数
	int maxseialnum;				//最大连续的空block数
	int maxseialnum_index;			//最大连续的空block数的起始地址
};


static void mem_modified(char *mem,int *maxseialnum,int *maxseialnum_index,int *emptynum){			//获得最大连续的空block数和起始地址和当前剩余空block数
	int num=0,temp=0;
	*maxseialnum_index=0;
	*maxseialnum=0;
	*emptynum=0;
	int i;
	for(i=257+1+2048;i<1024*1024;i++){
        if(mem[i]==0){
            if(num==0){temp=i;}
            num++;
            *emptynum=*emptynum+1;
        }
        if((i==1024*1024-1||mem[i]==1)&&num>*maxseialnum){
            *maxseialnum=num;
            *maxseialnum_index=temp;
            num=0;
            temp=0;
        }
        else if((i==1024*1024-1||mem[i]==1)&&num<=*maxseialnum){
            num=0;
            temp=0;
        }
    }
}

static void init_filenode(struct filenode *node){                   //初始化文件元信息结点
	int i;
	for(i=0;i<256;i++){
		node->name[i]='\0';
	}
	for(i=0;i<256;i++){
		node->content[i]=NULL;
	}
	for(i=0;i<256;i++){
		node->usedblock[i][0]=0;
		node->usedblock[i][1]=0;
	}
	node->next=NULL;
}

static void create_filenode(const char *name, struct stat *st){                 //创建文件结点
	//每一个文件的元信息使用2个block
	struct filenode *new = mmap(NULL, 2*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	init_filenode(new);
	memcpy(new->name, name, strlen(name) + 1);
	memcpy(&new->st,st,sizeof(struct stat));
	new->st.st_blksize=blocksize;
	new->st.st_blocks=2;
	new->next=fs_metadata->root;
	fs_metadata->root=new;
    //修改存储空间使用情况
	fs_metadata->memusage->mem[fs_metadata->memusage->maxseialnum_index]=1;
	fs_metadata->memusage->mem[fs_metadata->memusage->maxseialnum_index+1]=1;
    //建立块指针
    fs_metadata->memusage->mempoint[fs_metadata->memusage->maxseialnum_index]=new;
    fs_metadata->memusage->mempoint[fs_metadata->memusage->maxseialnum_index+1]=new+blocksize*sizeof(char);
	new->usedblock[256][0]=2;
	new->usedblock[256][1]=fs_metadata->memusage->maxseialnum_index;
	mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
}

static struct filenode *get_filenode(const char *name){             //查找文件结点
	struct filenode *node = fs_metadata->root;
    while(node) {
        if(strcmp(node->name, name+1) != 0)
            node = node->next;
        else
            return node;
    }
    return NULL;
}


static int simplefs_getattr(const char *path, struct stat *stbuf)                   //获得文件信息
{
    int ret = 0;
    struct filenode *node = get_filenode(path);
    if(strcmp(path, "/") == 0) {
        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_mode = S_IFDIR | 0755;
    } else if(node) {
        memcpy(stbuf, &node->st, sizeof(struct stat));
    } else {
        ret = -ENOENT;
    }
    return ret;
}

static int simplefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)           //读取目录
{
    struct filenode *node = fs_metadata->root;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    while(node) {
        filler(buf, node->name, &node->st, 0);
        node = node->next;
    }
    return 0;
}

static int simplefs_mknod(const char *path, mode_t mode, dev_t dev)                 
{
    struct stat st;
    st.st_mode = S_IFREG | 0644;
    st.st_uid = fuse_get_context()->uid;
    st.st_gid = fuse_get_context()->gid;
    st.st_nlink = 1;
    st.st_size = 0;
    create_filenode(path+1 , &st);
    return 0;
}

static int simplefs_open(const char *path, struct fuse_file_info *fi)   //打开文件
{
    return 0;
}

static int simplefs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct filenode *node = get_filenode(path);
    int i,j,k;
    int num,num2;
    int pre,pre2;
    int rest;
    int cnt;
    int extra;
    int index,index2;
    char *buff;
    char eof=-1;
    int offset2;
    for(j=0;j<256;j++){
    	if(node->content[j] == NULL){
    		break;
    	}
    }
    //第一次写文件
    if(j == 0){
    	if(fs_metadata->memusage->emptynum*blocksize >= size){
    		if(fs_metadata->memusage->maxseialnum*blocksize > size){
    			num=size/blocksize+1;
    			node->content[0]=mmap(NULL, num*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    			node->st.st_blocks+=num;
    			memcpy(node->content[0],buf,size);
    			for(i=fs_metadata->memusage->maxseialnum_index;i<num+fs_metadata->memusage->maxseialnum_index;i++){
    				fs_metadata->memusage->mem[i]=1;
                    fs_metadata->memusage->mempoint[i]=node->content[0]+(i-fs_metadata->memusage->maxseialnum_index)*blocksize*sizeof(char);
    			}
    			node->usedblock[0][0]=num;
    			node->usedblock[0][1]=fs_metadata->memusage->maxseialnum_index;
    			mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
    			i=0;
    			num=0;
    			rest=0;
    			cnt=0;
    		}
    		else if(fs_metadata->memusage->maxseialnum*blocksize <= size){
    			rest=size;
    			cnt=0;
    			while(rest>0){
    				if(fs_metadata->memusage->maxseialnum*blocksize <= rest){
    					node->content[cnt]=mmap(NULL, fs_metadata->memusage->maxseialnum*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    					node->st.st_blocks+=fs_metadata->memusage->maxseialnum;
    					memcpy(node->content[cnt],buf+size-rest,fs_metadata->memusage->maxseialnum*blocksize);
    					for(i=fs_metadata->memusage->maxseialnum_index;i<fs_metadata->memusage->maxseialnum+fs_metadata->memusage->maxseialnum_index;i++){
    						fs_metadata->memusage->mem[i]=1;
                            fs_metadata->memusage->mempoint[i]=node->content[cnt]+(i-fs_metadata->memusage->maxseialnum_index)*blocksize*sizeof(char);
    					}
    					node->usedblock[cnt][0]=fs_metadata->memusage->maxseialnum;
    					node->usedblock[cnt][1]=fs_metadata->memusage->maxseialnum_index;
    					mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
    					rest=rest-fs_metadata->memusage->maxseialnum*blocksize;
    					cnt++;
    				}
    				else if(fs_metadata->memusage->maxseialnum*blocksize > rest){
    					num=size/blocksize + 1;
    					node->content[cnt]=mmap(NULL, num*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    					node->st.st_blocks+=num;
    					memcpy(node->content[cnt],buf+size-rest,fs_metadata->memusage->maxseialnum*blocksize);
    					for(i=fs_metadata->memusage->maxseialnum_index;i<num+fs_metadata->memusage->maxseialnum_index;i++){
    						fs_metadata->memusage->mem[i]=1;
                            fs_metadata->memusage->mempoint[i]=node->content[cnt]+(i-fs_metadata->memusage->maxseialnum_index)*blocksize*sizeof(char);
    					}
    					node->usedblock[cnt][0]=num;
    					node->usedblock[cnt][1]=fs_metadata->memusage->maxseialnum_index;
    					mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
    					rest=rest-fs_metadata->memusage->maxseialnum*blocksize;
    					cnt++;
    				}
    			}
    			rest=0;
    			cnt=0;
    			i=0;
    			num=0;
    		}
    	}

    }
    //非首次写文件
    else if(j != 0){
        //保存文件offset前和offset+size后(如果有)的内容
		num=0;
    	pre=0;
    	for(i=0;i<256;i++){
    		if(node->content[i]!=NULL){
    			pre=num;
    			num=num+node->usedblock[i][0];
    		}
    		if(num*blocksize>=offset+1 && pre*blocksize < offset+1){
    			j=i;
    			break;
    		}
    	}
        index2=0;
        if(offset+size<node->st.st_size){
            offset2=offset+size;
            num2=0;
        pre2=0;
        for(i=0;i<256;i++){
            if(node->content[i]!=NULL){
                pre2=num2;
                num2=num2+node->usedblock[i][0];
            }
            if(num2*blocksize>=offset2+1 && pre2*blocksize < offset2+1){
                k=i;
                break;
            }
        }
        index2=offset2-pre2*blocksize;
        }
    	index=offset-pre*blocksize;
        if(index2 == 0){
            extra=index;
        }
        else{
            extra=index+node->st.st_size-index2;
        }
        extra=index;
    	buff=(char *)malloc(size+extra);
    	if(index>0){
    	memcpy(buff,node->content[j],index);}
    	memcpy(buff+index,buf,size);
        if(index2>0){
            rest=node->st.st_size-index2;
            cnt=k;
            if(rest<=node->usedblock[k][0]*blocksize-index2){
                memcpy(buff+index+size,node->content[k]+index2,rest);
            }
            else{
                memcpy(buff+index+size,node->content[k]+index2,node->usedblock[k][0]*blocksize-index2);
                rest=rest-(node->usedblock[k][0]*blocksize-index2);
                num2=index+size+node->usedblock[k][0]*blocksize-index2;
                cnt++;
                while(rest>=node->usedblock[cnt][0]*blocksize){
                    memcpy(buff+num2,node->content[cnt],node->usedblock[cnt][0]*blocksize);
                    rest=rest-node->usedblock[cnt][0]*blocksize;
                    num2=num2+node->usedblock[cnt][0]*blocksize;
                    cnt++;
                }
                if(rest>0){
                    memcpy(buff+num2,node->content[cnt],rest);
                }
            }
        }
        //将原文件offset所在块及后面所有块全部销毁重写
        for(i=j;i<256;i++){
        if(node->content[i]!=NULL){
        munmap(node->content[i],node->usedblock[i][0]*blocksize);
        node->content[i]=NULL;
        node->st.st_blocks-=node->usedblock[j][0];
        for(k=0;k<node->usedblock[i][0];i++){
            fs_metadata->memusage->mem[node->usedblock[i][1]+k]=0;
            fs_metadata->memusage->mempoint[node->usedblock[i][1]+k]=NULL;
        }
        node->usedblock[i][0]=0;
        node->usedblock[i][1]=0;
        }
        }
    	mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
    	if(fs_metadata->memusage->emptynum*blocksize >= size+extra){
    		if(fs_metadata->memusage->maxseialnum*blocksize > size+extra){
    			num=(size+extra)/blocksize+1;
    			node->content[j]=mmap(NULL, num*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    			node->st.st_blocks+=num;
    			memcpy(node->content[j],buff,size+extra);
    			for(i=fs_metadata->memusage->maxseialnum_index;i<num+fs_metadata->memusage->maxseialnum_index;i++){
    				fs_metadata->memusage->mem[i]=1;
                    fs_metadata->memusage->mempoint[i]=node->content[j]+(i-fs_metadata->memusage->maxseialnum_index)*blocksize*sizeof(char);
    			}
    			node->usedblock[j][0]=num;
    			node->usedblock[j][1]=fs_metadata->memusage->maxseialnum_index;
    			mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
    			i=0;
    			num=0;
    			rest=0;
    			cnt=0;
    		}
    		else if(fs_metadata->memusage->maxseialnum*blocksize <= size+extra){
    			rest=size+extra;
    			cnt=j+1;
    			while(rest>0){
    				if(fs_metadata->memusage->maxseialnum*blocksize <= rest){
    					node->content[cnt]=mmap(NULL, fs_metadata->memusage->maxseialnum*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    					node->st.st_blocks+=fs_metadata->memusage->maxseialnum;
    					memcpy(node->content[cnt],buff+size+extra-rest,fs_metadata->memusage->maxseialnum*blocksize);
    					for(i=fs_metadata->memusage->maxseialnum_index;i<fs_metadata->memusage->maxseialnum+fs_metadata->memusage->maxseialnum_index;i++){
    						fs_metadata->memusage->mem[i]=1;
                            fs_metadata->memusage->mempoint[i]=node->content[cnt]+(i-fs_metadata->memusage->maxseialnum_index)*blocksize*sizeof(char);
    					}
    					node->usedblock[cnt][0]=fs_metadata->memusage->maxseialnum;
    					node->usedblock[cnt][1]=fs_metadata->memusage->maxseialnum_index;
    					mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
    					rest=rest-fs_metadata->memusage->maxseialnum*blocksize;
    					cnt++;
    				}
    				else if(fs_metadata->memusage->maxseialnum*blocksize > rest){
    					num=size/blocksize + 1;
    					node->content[cnt]=mmap(NULL, num*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    					node->st.st_blocks+=num;
    					memcpy(node->content[cnt],buff+size+extra-rest,fs_metadata->memusage->maxseialnum*blocksize);
    					for(i=fs_metadata->memusage->maxseialnum_index;i<num+fs_metadata->memusage->maxseialnum_index;i++){
    						fs_metadata->memusage->mem[i]=1;
                            fs_metadata->memusage->mempoint[i]=node->content[cnt]+(i-fs_metadata->memusage->maxseialnum_index)*blocksize*sizeof(char);
    					}
    					node->usedblock[cnt][0]=num;
    					node->usedblock[cnt][1]=fs_metadata->memusage->maxseialnum_index;
    					mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
    					rest=rest-rest;
    					cnt++;
    				}
    			}
    			rest=0;
    			cnt=0;
    			i=0;
    			num=0;
    			extra=0;
    		}

			
    	}
    	free(buff);
    }
    //修改文件元信息(因为前面要用到原来的文件大小，所以在函数最后再修改文件大小)
    if(offset+size>node->st.st_size){
        node->st.st_size=offset+size;
    }
    return size;
}

static int simplefs_truncate(const char *path, off_t size){      //修改文件大小
	struct filenode *node = get_filenode(path);
	int i;
	int num=0;
	int j;
	int k;
	int cnt;
	int rest;
	int index;
	char *temp;
	int n;
	char eof=-1;
    node->st.st_size=size;
	for(i=0;i<256;i++){
		if(node->content[i] != NULL){
			num=num+node->usedblock[i][0];
		}
		else if(node->content[i] == NULL){
			break;
		}
	}
    //缩小文件
	if(num*blocksize >= size+blocksize){
		n=0;
		for(j=0;;j++){
			if(size+j*blocksize>num*blocksize){
				break;
			}
		}
		n=j-1;
		if(node->usedblock[i-1][0]>n){
			temp=(char *)malloc((node->usedblock[i-1][0]-n)*blocksize);
			memcpy(temp,node->content[i-1],(node->usedblock[i-1][0]-n)*blocksize);
			munmap(node->content[i-1],node->usedblock[i-1][0]);
			node->content[i-1]=NULL;
			node->st.st_blocks-=node->usedblock[i-1][0];
			for(k=0;k<node->usedblock[i-1][0];k++){
				fs_metadata->memusage->mem[node->usedblock[i-1][1]+k]=0;
                fs_metadata->memusage->mempoint[node->usedblock[i-1][1]+k]=NULL;
			}
			mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
			node->content[i-1]=mmap(NULL, (node->usedblock[i-1][0]-n)*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			memcpy(node->content[i-1],temp,(node->usedblock[i-1][0]-n)*blocksize);
			node->usedblock[i-1][0]-=n;
			node->usedblock[i-1][1]=fs_metadata->memusage->maxseialnum_index;
			for(k=0;k<node->usedblock[i-1][0];k++){
				fs_metadata->memusage->mem[node->usedblock[i-1][1]+k]=0;
                fs_metadata->memusage->mempoint[node->usedblock[i-1][1]+k]=NULL;
			}
			node->st.st_blocks+=node->usedblock[i-1][0];
			mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
		}
		else if(node->usedblock[i-1][0] == n){
			munmap(node->content[i-1],node->usedblock[i-1][0]);
			node->content[i-1]=NULL;
			node->st.st_blocks-=node->usedblock[i-1][0];
			node->usedblock[i-1][0]=0;
			node->usedblock[i-1][1]=0;
			for(k=0;k<node->usedblock[i-1][0];k++){
				fs_metadata->memusage->mem[node->usedblock[i-1][1]+k]=0;
                fs_metadata->memusage->mempoint[node->usedblock[i-1][1]+k]=NULL;
			}
			mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
		}
		else{
			for(j=i-1;j>-1;j--){
				n-=node->usedblock[j][0];
				if(n<=0){
					break;
				}
				else{
					munmap(node->content[j],node->usedblock[j][0]);
					node->content[j]=NULL;
					node->st.st_blocks-=node->usedblock[j][0];
					for(k=0;k<node->usedblock[j][0];k++){
						fs_metadata->memusage->mem[node->usedblock[j][1]+k]=0;
                        fs_metadata->memusage->mempoint[node->usedblock[j][1]+k]=NULL;
					}
					node->usedblock[j][0]=0;
					node->usedblock[j][1]=0;
				}
			}
			mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
			if(n == 0){
				munmap(node->content[j],node->usedblock[j][0]);
				node->content[j]=NULL;
					node->st.st_blocks-=node->usedblock[j][0];
					for(k=0;k<node->usedblock[j][0];k++){
						fs_metadata->memusage->mem[node->usedblock[j][1]+k]=0;
                        fs_metadata->memusage->mempoint[node->usedblock[j][1]+k]=NULL;
					}
					node->usedblock[j][0]=0;
					node->usedblock[j][1]=0;
				mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
			}
			else if(n<0){
				n+=node->usedblock[j][0];
				temp=(char *)malloc((node->usedblock[j][0]-n)*blocksize);
				memcpy(temp,node->content[j],(node->usedblock[j][0]-n)*blocksize);
				munmap(node->content[j],node->usedblock[j][0]);
				node->content[j]=NULL;
				node->st.st_blocks-=node->usedblock[j][0];
				for(k=0;k<node->usedblock[j][0];k++){
					fs_metadata->memusage->mem[node->usedblock[j][1]+k]=0;
                    fs_metadata->memusage->mempoint[node->usedblock[j][1]+k]=NULL;
				}
				mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
				node->content[j]=mmap(NULL, (node->usedblock[j][0]-n)*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
				memcpy(node->content[j],temp,(node->usedblock[j][0]-n)*blocksize);
				node->usedblock[j][0]-=n;
				node->usedblock[j][1]=fs_metadata->memusage->maxseialnum_index;
				for(k=0;k<node->usedblock[j][0];k++){
					fs_metadata->memusage->mem[node->usedblock[j][1]+k]=0;
                    fs_metadata->memusage->mempoint[node->usedblock[j][1]+k]=NULL;
				}
				node->st.st_blocks+=node->usedblock[j][0];
				mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);

			}
		}
	}
    //扩大文件
	else if(num*blocksize < size){
		cnt=i;
		for(j=0;;j++){
			if((num*blocksize+j*blocksize)>=size){
				break;
			}
		}
		if(j <= fs_metadata->memusage->maxseialnum){
			node->content[i]=mmap(NULL, j*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			node->st.st_blocks+=j;
			for(k=0;k<j;k++){
				fs_metadata->memusage->mem[fs_metadata->memusage->maxseialnum_index+k]=1;
                fs_metadata->memusage->mempoint[fs_metadata->memusage->maxseialnum_index+k]=node->content[i]+k*blocksize*sizeof(char);
			}
			node->usedblock[i][0]=j;
			node->usedblock[i][1]=fs_metadata->memusage->maxseialnum_index;
			mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
		}
		else if(j >fs_metadata->memusage->maxseialnum){
			rest=j;
			while(rest>0){
			node->content[cnt]=mmap(NULL, fs_metadata->memusage->maxseialnum*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			node->st.st_blocks+=fs_metadata->memusage->maxseialnum;
			for(k=0;k<fs_metadata->memusage->maxseialnum;k++){
				fs_metadata->memusage->mem[fs_metadata->memusage->maxseialnum_index+k]=1;
                fs_metadata->memusage->mempoint[fs_metadata->memusage->maxseialnum_index+k]=node->content[cnt]+k*blocksize*sizeof(char);
			}
			node->usedblock[cnt][0]=fs_metadata->memusage->maxseialnum;
			node->usedblock[cnt][1]=fs_metadata->memusage->maxseialnum_index;
			rest-=fs_metadata->memusage->maxseialnum;
			mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
			}
		}
		}
	return 0;
	}


static int simplefs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)   //读取文件
{
    struct filenode *node = get_filenode(path);
    int ret = size;
    int num=0;
    int i,j,k;
    int rest;
    int index;
    if(offset + size > node->st.st_size)
        ret = node->st.st_size - offset;
    for(i=0;i<256;i++){
    	if(node->content[i] == NULL){
    		break;
    	}
    }
    if(i == 1){
    	memcpy(buf,node->content[0]+offset, ret);
    }
    else if (i >1){
    	rest=offset;
    	for(j=0;;j++){
    		if(rest-node->usedblock[j][0]*blocksize < 0){
    			break;
    		}
    		rest=rest-node->usedblock[j][0]*blocksize;
    	}
    	index=rest+node->usedblock[j][0]*blocksize;
    	rest=ret;
    	if(node->usedblock[j][0]*blocksize-index >= rest){
    		memcpy(buf,node->content[j]+index,rest);
    	}
    	else{
    		memcpy(buf,node->content[j]+index,node->usedblock[j][0]*blocksize-index);
    		num=num+node->usedblock[j][0]*blocksize-index;
    		rest=rest-(node->usedblock[j][0]*blocksize-index);
    		k=j+1;
    		while(rest>=node->usedblock[k][0]*blocksize){
    			memcpy(buf+num,node->content[k],node->usedblock[k][0]*blocksize);
    			num+=node->usedblock[k][0]*blocksize;
    			rest-=node->usedblock[k][0]*blocksize;
    			k++;
    		}
    		if(rest>0){
    			memcpy(buf+num,node->content[k],rest);
    		}


    	}
    	
    }
    return ret;
}



static int simplefs_unlink(const char *path)       //删除文件
{
    struct filenode *node = get_filenode(path);
    struct filenode *p = fs_metadata->root;
    int i,j;
    if(p == node){
    	fs_metadata->root=node->next;
    }
    else{
    	while (p->next != node){
    		p=p->next;
    	}
    	p->next=node->next;
    }
    for(i=0;i<256;i++){
    	if(node->content[i] != NULL){
    		for(j=0;j<node->usedblock[i][0];j++){
    			fs_metadata->memusage->mem[node->usedblock[i][1]+j]=0;
                fs_metadata->memusage->mempoint[node->usedblock[i][1]+j]=NULL;
    		}
    		munmap(node->content[i],node->usedblock[i][0]*blocksize);
    		node->content[i]=NULL;
    	}
    	else{
    		break;
    	}
    }
    fs_metadata->memusage->mem[node->usedblock[256][1]]=0;
    fs_metadata->memusage->mem[node->usedblock[256][1]+1]=0;
    munmap(node,2*blocksize);
    node=NULL;
    mem_modified(fs_metadata->memusage->mem,&fs_metadata->memusage->maxseialnum,&fs_metadata->memusage->maxseialnum_index,&fs_metadata->memusage->emptynum);
    return 0;
}



static void *simplefs_init(struct fuse_conn_info *conn){			  //文件系统初始化
	//第一块block存放文件系统的元数据
	fs_metadata = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	fs_metadata->fs_size=size;
	fs_metadata->fs_blocksize=blocksize;
	fs_metadata->fs_blocknr=blocknr;
	fs_metadata->root=NULL;
	//第2~2306块block存放文件系统block情况
	fs_metadata->memusage = mmap(NULL, (257+2048)*blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int i;
    //块指针修改
    fs_metadata->memusage->mempoint[0]=fs_metadata;
    for(i=1;i<257+2048;i++){
        fs_metadata->memusage->mempoint[i]=fs_metadata->memusage+(i-1)*sizeof(char);
    }
	for(i=0;i<257+1+2048;i++){
		fs_metadata->memusage->mem[i]=1;
	}
	for(i=257+1+2048;i<1024*1024;i++){
		fs_metadata->memusage->mem[i]=0;
	}
	fs_metadata->memusage->emptynum=1024*1024-257-1;
	fs_metadata->memusage->maxseialnum=257+1;
}




static const struct fuse_operations op = {
    .init = simplefs_init,
    .getattr = simplefs_getattr,
    .readdir = simplefs_readdir,
    .mknod = simplefs_mknod,
    .open = simplefs_open,
    .write = simplefs_write,
    .truncate = simplefs_truncate,
    .read = simplefs_read,
    .unlink = simplefs_unlink,
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &op, NULL);
}
