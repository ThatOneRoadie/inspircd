/*		 +------------------------------------+
 *		 | Inspire Internet Relay Chat Daemon |
 *		 +------------------------------------+
 *
 *	InspIRCd: (C) 2002-2008 InspIRCd Development Team
 * See: http://www.inspircd.org/wiki/index.php/Credits
 *
 * This program is free but copyrighted software; see
 *			  the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"
#include <tds.h>
#include <tdsconvert.h>
#include "users.h"
#include "channels.h"
#include "modules.h"

#include "m_sqlv2.h"

/* $ModDesc: MsSQL provider */
/* $CompileFlags: exec("cat /usr/include/tdsver.h | grep VERSION_NO | perl -e 'if (<> =~ /freetds v([0-9]+\.[0-9]+)/i) { print "-D_TDSVER=".$1*100} else { print "-D_TDSVER=0" }'") */
/* $LinkerFlags: -ltds */
/* $ModDep: m_sqlv2.h */

class SQLConn;
class MsSQLResult;
class ResultNotifier;
class MsSQLListener;
class ModuleMsSQL;

typedef std::map<std::string, SQLConn*> ConnMap;
typedef std::deque<MsSQLResult*> ResultQueue;

ResultNotifier* notifier = NULL;
MsSQLListener* listener = NULL;
int QueueFD = -1;

ConnMap connections;
Mutex* QueueMutex;
Mutex* ResultsMutex;
Mutex* LoggingMutex;

class QueryThread : public Thread
{
  private:
	ModuleMsSQL* Parent;
	InspIRCd* ServerInstance;
  public:
	QueryThread(InspIRCd* si, ModuleMsSQL* mod)
	: Thread(), Parent(mod), ServerInstance(si)
	{
	}
	~QueryThread() { }
	virtual void Run();
};

class ResultNotifier : public BufferedSocket
{
	ModuleMsSQL* mod;

 public:
	ResultNotifier(ModuleMsSQL* m, InspIRCd* SI, int newfd, char* ip) : BufferedSocket(SI, newfd, ip), mod(m)
	{
	}

	virtual bool OnDataReady()
	{
		char data = 0;
		if (ServerInstance->SE->Recv(this, &data, 1, 0) > 0)
		{
			Dispatch();
			return true;
		}
		return false;
	}

	void Dispatch();
};

class MsSQLListener : public ListenSocketBase
{
	ModuleMsSQL* Parent;
	irc::sockets::insp_sockaddr sock_us;
	socklen_t uslen;
	FileReader* index;

 public:
	MsSQLListener(ModuleMsSQL* P, InspIRCd* Instance, int port, const std::string &addr) : ListenSocketBase(Instance, port, addr), Parent(P)
	{
		uslen = sizeof(sock_us);
		if (getsockname(this->fd,(sockaddr*)&sock_us,&uslen))
		{
			throw ModuleException("Could not getsockname() to find out port number for ITC port");
		}
	}

	virtual void OnAcceptReady(const std::string &ipconnectedto, int nfd, const std::string &incomingip)
	{
		new ResultNotifier(this->Parent, this->ServerInstance, nfd, (char *)ipconnectedto.c_str()); // XXX unsafe casts suck
	}

	/* Using getsockname and ntohs, we can determine which port number we were allocated */
	int GetPort()
	{
#ifdef IPV6
		return ntohs(sock_us.sin6_port);
#else
		return ntohs(sock_us.sin_port);
#endif
	}
};


class MsSQLResult : public SQLresult
{
 private:
	int currentrow;
	int rows;
	int cols;

	std::vector<std::string> colnames;
	std::vector<SQLfieldList> fieldlists;
	SQLfieldList emptyfieldlist;

	SQLfieldList* fieldlist;
	SQLfieldMap* fieldmap;

 public:
	MsSQLResult(Module* self, Module* to, unsigned int rid)
	: SQLresult(self, to, rid), currentrow(0), rows(0), cols(0), fieldlist(NULL), fieldmap(NULL)
	{
	}

	~MsSQLResult()
	{
	}

	void AddRow(int colsnum, char **dat, char **colname)
	{
		colnames.clear();
		cols = colsnum;
		for (int i = 0; i < colsnum; i++)
		{
			fieldlists.resize(fieldlists.size()+1);
			colnames.push_back(colname[i]);
			SQLfield sf(dat[i] ? dat[i] : "", dat[i] ? false : true);
			fieldlists[rows].push_back(sf);
		}
		rows++;
	}

	void UpdateAffectedCount()
	{
		rows++;
	}

	virtual int Rows()
	{
		return rows;
	}

	virtual int Cols()
	{
		return cols;
	}

	virtual std::string ColName(int column)
	{
		if (column < (int)colnames.size())
		{
			return colnames[column];
		}
		else
		{
			throw SQLbadColName();
		}
		return "";
	}

	virtual int ColNum(const std::string &column)
	{
		for (unsigned int i = 0; i < colnames.size(); i++)
		{
			if (column == colnames[i])
				return i;
		}
		throw SQLbadColName();
		return 0;
	}

	virtual SQLfield GetValue(int row, int column)
	{
		if ((row >= 0) && (row < rows) && (column >= 0) && (column < Cols()))
		{
			return fieldlists[row][column];
		}

		throw SQLbadColName();

		/* XXX: We never actually get here because of the throw */
		return SQLfield("",true);
	}

	virtual SQLfieldList& GetRow()
	{
		if (currentrow < rows)
			return fieldlists[currentrow];
		else
			return emptyfieldlist;
	}

	virtual SQLfieldMap& GetRowMap()
	{
		/* In an effort to reduce overhead we don't actually allocate the map
		 * until the first time it's needed...so...
		 */
		if(fieldmap)
		{
			fieldmap->clear();
		}
		else
		{
			fieldmap = new SQLfieldMap;
		}

		if (currentrow < rows)
		{
			for (int i = 0; i < Cols(); i++)
			{
				fieldmap->insert(std::make_pair(ColName(i), GetValue(currentrow, i)));
			}
			currentrow++;
		}

		return *fieldmap;
	}

	virtual SQLfieldList* GetRowPtr()
	{
		fieldlist = new SQLfieldList();

		if (currentrow < rows)
		{
			for (int i = 0; i < Rows(); i++)
			{
				fieldlist->push_back(fieldlists[currentrow][i]);
			}
			currentrow++;
		}
		return fieldlist;
	}

	virtual SQLfieldMap* GetRowMapPtr()
	{
		fieldmap = new SQLfieldMap();

		if (currentrow < rows)
		{
			for (int i = 0; i < Cols(); i++)
			{
				fieldmap->insert(std::make_pair(colnames[i],GetValue(currentrow, i)));
			}
			currentrow++;
		}

		return fieldmap;
	}

	virtual void Free(SQLfieldMap* fm)
	{
		delete fm;
	}

	virtual void Free(SQLfieldList* fl)
	{
		delete fl;
	}


};

class SQLConn : public classbase
{
 private:
	ResultQueue results;
	InspIRCd* ServerInstance;
	Module* mod;
	SQLhost host;
	TDSLOGIN* login;
	TDSSOCKET* sock;
	TDSCONTEXT* context;

 public:
	QueryQueue queue;

	SQLConn(InspIRCd* SI, Module* m, const SQLhost& hi)
	: ServerInstance(SI), mod(m), host(hi), login(NULL), sock(NULL), context(NULL)
	{
		if (OpenDB())
		{
			std::string query("USE " + host.name);
			if (tds_submit_query(sock, query.c_str()) == TDS_SUCCEED)
			{
				if (tds_process_simple_query(sock) != TDS_SUCCEED)
				{
					LoggingMutex->Lock();
					ServerInstance->Logs->Log("m_mssql",DEFAULT, "WARNING: Could not select database " + host.name + " for DB with id: " + host.id);
					LoggingMutex->Unlock();
					CloseDB();
				}
			}
			else
			{
				LoggingMutex->Lock();
				ServerInstance->Logs->Log("m_mssql",DEFAULT, "WARNING: Could not select database " + host.name + " for DB with id: " + host.id);
				LoggingMutex->Unlock();
				CloseDB();
			}
		}
		else
		{
			LoggingMutex->Lock();
			ServerInstance->Logs->Log("m_mssql",DEFAULT, "WARNING: Could not connect to DB with id: " + host.id);
			LoggingMutex->Unlock();
			CloseDB();
		}
	}

	~SQLConn()
	{
		CloseDB();
	}

	SQLerror Query(SQLrequest &req)
	{
		if (!sock)
			return SQLerror(SQL_BAD_CONN, "Socket was NULL, check if SQL server is running.");

		/* Pointer to the buffer we screw around with substitution in */
		char* query;

		/* Pointer to the current end of query, where we append new stuff */
		char* queryend;

		/* Total length of the unescaped parameters */
		unsigned long paramlen;

		/* Total length of query, used for binary-safety */
		unsigned long querylength = 0;

		paramlen = 0;
		for(ParamL::iterator i = req.query.p.begin(); i != req.query.p.end(); i++)
		{
			paramlen += i->size();
		}

		/* To avoid a lot of allocations, allocate enough memory for the biggest the escaped query could possibly be.
		 * sizeofquery + (totalparamlength*2) + 1
		 *
		 * The +1 is for null-terminating the string
		 */
		query = new char[req.query.q.length() + (paramlen*2) + 1];
		queryend = query;

		for(unsigned long i = 0; i < req.query.q.length(); i++)
		{
			if(req.query.q[i] == '?')
			{
				if(req.query.p.size())
				{
					/* Custom escaping for this one. converting ' to '' should make SQL Server happy. Ugly but fast :]
					 */
					char* escaped = new char[(req.query.p.front().length() * 2) + 1];
					char* escend = escaped;
					for (std::string::iterator p = req.query.p.front().begin(); p < req.query.p.front().end(); p++)
					{
						if (*p == '\'')
						{
							*escend = *p;
							escend++;
							*escend = *p;
						}
						*escend = *p;
						escend++;
					}
					*escend = 0;

					for (char* n = escaped; *n; n++)
					{
						*queryend = *n;
						queryend++;
					}
					delete[] escaped;
					req.query.p.pop_front();
				}
				else
					break;
			}
			else
			{
				*queryend = req.query.q[i];
				queryend++;
			}
			querylength++;
		}
		*queryend = 0;
		req.query.q = query;

		MsSQLResult* res = new MsSQLResult((Module*)mod, req.GetSource(), req.id);
		res->dbid = host.id;
		res->query = req.query.q;

		char* msquery = strdup(req.query.q.data());
		LoggingMutex->Lock();
		ServerInstance->Logs->Log("m_mssql",DEBUG,"doing Query: %s",msquery);
		LoggingMutex->Unlock();
		if (tds_submit_query(sock, msquery) != TDS_SUCCEED)
		{
			std::string error("failed to execute: "+std::string(req.query.q.data()));
			delete[] query;
			delete res;
			free(msquery);
			return SQLerror(SQL_QSEND_FAIL, error);
		}
		delete[] query;
		free(msquery);

		int tds_res;
		while (tds_process_tokens(sock, &tds_res, NULL, TDS_TOKEN_RESULTS) == TDS_SUCCEED)
		{
			//ServerInstance->Logs->Log("m_mssql",DEBUG,"<******> result type: %d", tds_res);
			//ServerInstance->Logs->Log("m_mssql",DEBUG,"AFFECTED ROWS: %d", sock->rows_affected);
			switch (tds_res)
			{
				case TDS_ROWFMT_RESULT:
					break;

				case TDS_DONE_RESULT:
					if (sock->rows_affected > -1)
					{
						for (int c = 0; c < sock->rows_affected; c++)  res->UpdateAffectedCount();
						continue;
					}
					break;

				case TDS_ROW_RESULT:
					while (tds_process_tokens(sock, &tds_res, NULL, TDS_STOPAT_ROWFMT|TDS_RETURN_DONE|TDS_RETURN_ROW) == TDS_SUCCEED)
					{
						if (tds_res != TDS_ROW_RESULT)
							break;

						if (!sock->current_results)
							continue;

						if (sock->res_info->row_count > 0)
						{
							int cols = sock->res_info->num_cols;
							char** name = new char*[MAXBUF];
							char** data = new char*[MAXBUF];
							for (int j=0; j<cols; j++)
							{
								TDSCOLUMN* col = sock->current_results->columns[j];
								name[j] = col->column_name;

								int ctype;
								int srclen;
								unsigned char* src;
								CONV_RESULT dres;
								ctype = tds_get_conversion_type(col->column_type, col->column_size);
#if _TDSVER >= 82
									src = col->column_data;
#else
									src = &(sock->current_results->current_row[col->column_offset]);
#endif
								srclen = col->column_cur_size;
								tds_convert(sock->tds_ctx, ctype, (TDS_CHAR *) src, srclen, SYBCHAR, &dres);
								data[j] = (char*)dres.ib;
							}
							ResultReady(res, cols, data, name);
						}
					}
					break;

				default:
					break;
			}
		}
		ResultsMutex->Lock();
		results.push_back(res);
		ResultsMutex->Unlock();
		SendNotify();
		return SQLerror();
	}

	static int HandleMessage(const TDSCONTEXT * pContext, TDSSOCKET * pTdsSocket, TDSMESSAGE * pMessage)
	{
		SQLConn* sc = (SQLConn*)pContext->parent;
		LoggingMutex->Lock();
		sc->ServerInstance->Logs->Log("m_mssql", DEBUG, "Message for DB with id: %s -> %s", sc->host.id.c_str(), pMessage->message);
		LoggingMutex->Unlock();
		return 0;
	}

	static int HandleError(const TDSCONTEXT * pContext, TDSSOCKET * pTdsSocket, TDSMESSAGE * pMessage)
	{
		SQLConn* sc = (SQLConn*)pContext->parent;
		LoggingMutex->Lock();
		sc->ServerInstance->Logs->Log("m_mssql", DEFAULT, "Error for DB with id: %s -> %s", sc->host.id.c_str(), pMessage->message);
		LoggingMutex->Unlock();
		return 0;
	}

	void ResultReady(MsSQLResult *res, int cols, char **data, char **colnames)
	{
		res->AddRow(cols, data, colnames);
	}

	void AffectedReady(MsSQLResult *res)
	{
		res->UpdateAffectedCount();
	}

	bool OpenDB()
	{
		CloseDB();

		TDSCONNECTION* conn = NULL;

		login = tds_alloc_login();
		tds_set_app(login, "TSQL");
		tds_set_library(login,"TDS-Library");
		tds_set_host(login, "");
		tds_set_server(login, host.host.c_str());
		tds_set_server_addr(login, host.host.c_str());
		tds_set_user(login, host.user.c_str());
		tds_set_passwd(login, host.pass.c_str());
		tds_set_port(login, host.port);
		tds_set_packet(login, 512);

		context = tds_alloc_context(this);
		context->msg_handler = HandleMessage;
		context->err_handler = HandleError;

		sock = tds_alloc_socket(context, 512);
		tds_set_parent(sock, NULL);

		conn = tds_read_config_info(NULL, login, context->locale);

		if (tds_connect(sock, conn) == TDS_SUCCEED)
		{
			tds_free_connection(conn);
			return 1;
		}
		tds_free_connection(conn);
		return 0;
	}

	void CloseDB()
	{
		if (sock)
		{
			tds_free_socket(sock);
			sock = NULL;
		}
		if (context)
		{
			tds_free_context(context);
			context = NULL;
		}
		if (login)
		{
			tds_free_login(login);
			login = NULL;
		}
	}

	SQLhost GetConfHost()
	{
		return host;
	}

	void SendResults()
	{
		while (results.size())
		{
			MsSQLResult* res = results[0];
			ResultsMutex->Lock();
			if (res->GetDest())
			{
				res->Send();
			}
			else
			{
				/* If the client module is unloaded partway through a query then the provider will set
				 * the pointer to NULL. We cannot just cancel the query as the result will still come
				 * through at some point...and it could get messy if we play with invalid pointers...
				 */
				delete res;
			}
			results.pop_front();
			ResultsMutex->Unlock();
		}
	}

	void ClearResults()
	{
		while (results.size())
		{
			MsSQLResult* res = results[0];
			delete res;
			results.pop_front();
		}
	}

	void SendNotify()
	{
		if (QueueFD < 0)
		{
			if ((QueueFD = socket(AF_FAMILY, SOCK_STREAM, 0)) == -1)
			{
				/* crap, we're out of sockets... */
				return;
			}

			irc::sockets::insp_sockaddr addr;

#ifdef IPV6
			irc::sockets::insp_aton("::1", &addr.sin6_addr);
			addr.sin6_family = AF_FAMILY;
			addr.sin6_port = htons(listener->GetPort());
#else
			irc::sockets::insp_inaddr ia;
			irc::sockets::insp_aton("127.0.0.1", &ia);
			addr.sin_family = AF_FAMILY;
			addr.sin_addr = ia;
			addr.sin_port = htons(listener->GetPort());
#endif

			if (connect(QueueFD, (sockaddr*)&addr,sizeof(addr)) == -1)
			{
				/* wtf, we cant connect to it, but we just created it! */
				return;
			}
		}
		char id = 0;
		send(QueueFD, &id, 1, 0);
	}

	void DoLeadingQuery()
	{
		SQLrequest& req = queue.front();
		req.error = Query(req);
	}

};


class ModuleMsSQL : public Module
{
 private:
	unsigned long currid;
	QueryThread* queryDispatcher;

 public:
	ModuleMsSQL(InspIRCd* Me)
	: Module(Me), currid(0)
	{
		LoggingMutex = ServerInstance->Mutexes->CreateMutex();
		ResultsMutex = ServerInstance->Mutexes->CreateMutex();
		QueueMutex = ServerInstance->Mutexes->CreateMutex();

		ServerInstance->Modules->UseInterface("SQLutils");

		if (!ServerInstance->Modules->PublishFeature("SQL", this))
		{
			throw ModuleException("m_mssql: Unable to publish feature 'SQL'");
		}

		/* Create a socket on a random port. Let the tcp stack allocate us an available port */
#ifdef IPV6
		listener = new MsSQLListener(this, ServerInstance, 0, "::1");
#else
		listener = new MsSQLListener(this, ServerInstance, 0, "127.0.0.1");
#endif

		if (listener->GetFd() == -1)
		{
			ServerInstance->Modules->DoneWithInterface("SQLutils");
			throw ModuleException("m_mssql: unable to create ITC pipe");
		}
		else
		{
			LoggingMutex->Lock();
			ServerInstance->Logs->Log("m_mssql", DEBUG, "MsSQL: Interthread comms port is %d", listener->GetPort());
			LoggingMutex->Unlock();
		}

		ReadConf();

		queryDispatcher = new QueryThread(ServerInstance, this);
		ServerInstance->Threads->Create(queryDispatcher);

		ServerInstance->Modules->PublishInterface("SQL", this);
		Implementation eventlist[] = { I_OnRequest, I_OnRehash };
		ServerInstance->Modules->Attach(eventlist, this, 2);
	}

	virtual ~ModuleMsSQL()
	{
		delete queryDispatcher;
		ClearQueue();
		ClearAllConnections();

		ServerInstance->SE->DelFd(listener);
		ServerInstance->BufferedSocketCull();

		if (QueueFD >= 0)
		{
			shutdown(QueueFD, 2);
			close(QueueFD);
		}

		if (notifier)
		{
			ServerInstance->SE->DelFd(notifier);
			notifier->Close();
			ServerInstance->BufferedSocketCull();
		}

		ServerInstance->Modules->UnpublishInterface("SQL", this);
		ServerInstance->Modules->UnpublishFeature("SQL");
		ServerInstance->Modules->DoneWithInterface("SQLutils");

		delete LoggingMutex;
		delete ResultsMutex;
		delete QueueMutex;
	}


	void SendQueue()
	{
		for (ConnMap::iterator iter = connections.begin(); iter != connections.end(); iter++)
		{
			iter->second->SendResults();
		}
	}

	void ClearQueue()
	{
		for (ConnMap::iterator iter = connections.begin(); iter != connections.end(); iter++)
		{
			iter->second->ClearResults();
		}
	}

	bool HasHost(const SQLhost &host)
	{
		for (ConnMap::iterator iter = connections.begin(); iter != connections.end(); iter++)
		{
			if (host == iter->second->GetConfHost())
				return true;
		}
		return false;
	}

	bool HostInConf(const SQLhost &h)
	{
		ConfigReader conf(ServerInstance);
		for(int i = 0; i < conf.Enumerate("database"); i++)
		{
			SQLhost host;
			host.id		= conf.ReadValue("database", "id", i);
			host.host	= conf.ReadValue("database", "hostname", i);
			host.port	= conf.ReadInteger("database", "port", "1433", i, true);
			host.name	= conf.ReadValue("database", "name", i);
			host.user	= conf.ReadValue("database", "username", i);
			host.pass	= conf.ReadValue("database", "password", i);
			if (h == host)
				return true;
		}
		return false;
	}

	void ReadConf()
	{
		ClearOldConnections();

		ConfigReader conf(ServerInstance);
		for(int i = 0; i < conf.Enumerate("database"); i++)
		{
			SQLhost host;

			host.id		= conf.ReadValue("database", "id", i);
			host.host	= conf.ReadValue("database", "hostname", i);
			host.port	= conf.ReadInteger("database", "port", "1433", i, true);
			host.name	= conf.ReadValue("database", "name", i);
			host.user	= conf.ReadValue("database", "username", i);
			host.pass	= conf.ReadValue("database", "password", i);

			if (HasHost(host))
				continue;

			this->AddConn(host);
		}
	}

	void AddConn(const SQLhost& hi)
	{
		if (HasHost(hi))
		{
			LoggingMutex->Lock();
			ServerInstance->Logs->Log("m_mssql",DEFAULT, "WARNING: A MsSQL connection with id: %s already exists. Aborting database open attempt.", hi.id.c_str());
			LoggingMutex->Unlock();
			return;
		}

		SQLConn* newconn;

		newconn = new SQLConn(ServerInstance, this, hi);

		connections.insert(std::make_pair(hi.id, newconn));
	}

	void ClearOldConnections()
	{
		ConnMap::iterator iter,safei;
		for (iter = connections.begin(); iter != connections.end(); iter++)
		{
			if (!HostInConf(iter->second->GetConfHost()))
			{
				delete iter->second;
				safei = iter;
				--iter;
				connections.erase(safei);
			}
		}
	}

	void ClearAllConnections()
	{
		ConnMap::iterator i;
		while ((i = connections.begin()) != connections.end())
		{
			connections.erase(i);
			delete i->second;
		}
	}

	virtual void OnRehash(User* user, const std::string &parameter)
	{
		QueueMutex->Lock();
		ReadConf();
		QueueMutex->Unlock();
	}

	virtual const char* OnRequest(Request* request)
	{
		if(strcmp(SQLREQID, request->GetId()) == 0)
		{
			SQLrequest* req = (SQLrequest*)request;

			QueueMutex->Lock();

			ConnMap::iterator iter;

			const char* returnval = NULL;

			if((iter = connections.find(req->dbid)) != connections.end())
			{
				req->id = NewID();
				iter->second->queue.push(*req);
				returnval= SQLSUCCESS;
			}
			else
			{
				req->error.Id(SQL_BAD_DBID);
			}

			QueueMutex->Unlock();

			return returnval;
		}
		return NULL;
	}

	unsigned long NewID()
	{
		if (currid+1 == 0)
			currid++;

		return ++currid;
	}

	virtual Version GetVersion()
	{
		return Version("$Id$", VF_VENDOR | VF_SERVICEPROVIDER, API_VERSION);
	}

};

void ResultNotifier::Dispatch()
{
	mod->SendQueue();
}

void QueryThread::Run()
{
	while (this->GetExitFlag() == false)
	{
		SQLConn* conn = NULL;
		QueueMutex->Lock();
		for (ConnMap::iterator i = connections.begin(); i != connections.end(); i++)
		{
			if (i->second->queue.totalsize())
			{
				conn = i->second;
				break;
			}
		}
		QueueMutex->Unlock();
		if (conn)
		{
			conn->DoLeadingQuery();
			QueueMutex->Lock();
			conn->queue.pop();
			QueueMutex->Unlock();
		}
		usleep(1000);
	}
}

MODULE_INIT(ModuleMsSQL)