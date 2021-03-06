#include "TableWindow.h"
#include "imgui.h"
#include "JsonReader.h"
#include "Util.h"
#include "HttpProtocol.h"
#include "TcpClient.h"
#include "Log.h"

#include <random>

#include "thread_pool.hpp"

//static thread_pool pool;

TableWindow::TableWindow()
{
    timer.reset();

    // Load previous export file saved
    std::string fileData = Util::FileToString("export.json");

    if (fileData.size() > 0)
    {
        JsonReader reader;
        JsonValue json;
        if (reader.ParseString(json, fileData))
        {
            TLogInfo("Importing export.json");
            mStartTime = json.FindValue("startTime").GetInteger64();
            JsonArray arr = json.FindValue("table").GetArray();

            for (const auto& a : arr)
            {
                Entry e;

                e.tag = a.FindValue("id").GetInteger64();
                e.FromString(a.FindValue("temps").GetString(), mStartTime);

                mTable[e.tag] = e;
            }
        }
    }

    RefreshWindowParameter();

    mHttpThread = std::thread(&TableWindow::RunHttp, this);
}

TableWindow::~TableWindow()
{
    HttpClient::Request req;
    req.quit = true;
    mHttpQueue.Push(req);

    if (mHttpThread.joinable())
    {
        mHttpThread.join();
    }
}

void TableWindow::RefreshWindowParameter()
{
    std::string tempWindow = std::to_string(mWindow/1000);
    sprintf(buf2, "%.10s", tempWindow.c_str());
}

void TableWindow::RunHttp()
{
    bool quit = false;
    HttpClient::Request req;
    while(!quit)
    {
        if (mHttpQueue.TryPop(req))
        {
            if (!req.quit)
            {
                std::string response = mHttpClient.ExecuteAsync(req);

                std::cout << response << std::endl;
                mSending = false;
            }
            quit = req.quit;
        }
    }
}

void TableWindow::SendToServer(const std::string &body)
{
    HttpClient::Request req;

    req.action = HttpClient::Action::HTTP_POST;
    req.host = mServer;
    req.port = std::to_string(mPort);
    req.body = body;
    req.target = mPath;
    req.secured = false;

    mHttpQueue.Push(req);



//    HttpRequest request;

//    request.method = "POST";
//    request.protocol = "HTTP/1.1";
//    request.query = path;
//    request.body = body;
//    request.headers["Host"] = "www." + host;
//    request.headers["Content-type"] = "application/json";
//    request.headers["Content-length"] = std::to_string(body.size());

//    tcp::TcpClient client;
//    HttpProtocol http;

//    client.Initialize();
//    if (client.Connect(host, port))
//    {
//        if (client.Send(http.GenerateRequest(request)))
//        {
//            TLogInfo("[HTTP] Send request success!");
//        }
//        else
//        {
//            TLogError("[HTTP] Send request failed");
//        }
//    }
//    else
//    {
//        TLogError("[HTTP] Connect to server failed");
//    }
}

std::string TableWindow::ToJson(const std::map<int64_t, Entry> &table, int64_t startTime)
{
    JsonArray arr;

    for (const auto &t : table)
    {
        JsonObject obj;

        obj.AddValue("id", t.first);
        obj.AddValue("tours", static_cast<uint32_t>(t.second.laps.size()));
        obj.AddValue("temps", t.second.ToString(startTime));
        arr.AddValue(obj);
    }
    return arr.ToString();
}


void TableWindow::Autosave(const std::map<int64_t, Entry>& table, int64_t startTime)
{
    JsonObject json;
    JsonArray arr;

    for (const auto& t : table)
    {
        JsonObject obj;

        obj.AddValue("id", t.first);
        obj.AddValue("tours", static_cast<uint32_t>(t.second.laps.size()));
        obj.AddValue("temps", t.second.ToString(startTime));
        arr.AddValue(obj);
    }

    json.AddValue("startTime", startTime);
    json.AddValue("table", arr);

    Util::StringToFile("export.json", json.ToString(), false);
}

bool TableWindow::ShowEraseConfirm()
{
    bool quitRequest = false;
    // Always center this window when appearing
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
   // ImGui::SetNextWindowSize(ImVec2(200, 150));
    if (ImGui::BeginPopupModal("EraseConfirm", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Voulez-vous vraiment effacer les r??sultats ?");
        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(120, 0)))
        {
            quitRequest = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return quitRequest;
}

void TableWindow::Draw(const char *title, bool *p_open, IProcessEngine &engine)
{
    (void) p_open;

    // Local copy to shorter mutex locking
    mMutex.lock();
    std::map<int64_t, Entry> table = mTable;
    int64_t startTime = mStartTime;
    mMutex.unlock();

    ImGui::Begin(title, nullptr);

    ImGui::Text("Tableau des passages");

    if (timer.elapsed() > 10)
    {
        // Auto save
        timer.reset();
        Autosave(table, startTime);
        TLogInfo("Auto-save file export.json in: " + Util::GetWorkingDirectory());

        // Auto send to cloud
        mSendToCloud = true;
    }

    /* ======================  Envoi dans le Cloud ====================== */
    if (ImGui::Button( "Envoyer", ImVec2(100, 40)) || mSendToCloud)
    {
        mSendToCloud = false;
        if (!mSending)
        {
            mSending = true;
            mMutex.lock();
            std::map<int64_t, Entry> tableCopy = mTable;
            mMutex.unlock();
            SendToServer(ToJson(tableCopy, startTime));
/*
            pool.push_task([&] () {
          //  mPool.enqueue_task([&] () {
                mMutex.lock();
                std::map<int64_t, Entry> tableCopy = mTable;
                mMutex.unlock();

               SendToServer(ToJson(tableCopy, startTime), mServer, mPath, mPort);
               sendInAction = false;
            });
            */
        }
    }

    ImGui::SameLine(400.0);


    if (ImGui::Button( "EFFACER", ImVec2(100, 40)))
    {
        ImGui::OpenPopup("EraseConfirm");
    }

    if (ShowEraseConfirm())
    {
        std::scoped_lock<std::mutex> lock(mMutex);
        mTable.clear();
        Autosave(table, startTime);
    }


    ImGui::Text("Tags : %d", static_cast<int>(table.size()));

    /* ======================  PARAMETRAGE ====================== */
    ImGui::PushItemWidth(200);
    ImGui::InputText("Fen??tre de blocage (en secondes)",  buf2, sizeof(buf2), ImGuiInputTextFlags_CharsDecimal);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button( "Appliquer", ImVec2(100, 40)))
    {
       std::scoped_lock<std::mutex> lock(mMutex);
       mWindow = Util::FromString<int64_t>(buf2) * 1000; // en millisecondes
    }

    /* ======================  CATEGORIES ====================== */
    uint32_t nbLines = engine.GetTableSize("categories");
    if (nbLines == 1)
    {
        if (engine.GetTableEntry("categories", 0, mCatLabels))
        {
            // Les cat??gories ont chang??
            if (mCatLabels.size() != mCategories.size())
            {
                mCategories.clear();
                for (const auto & c : mCatLabels)
                {
                    mCategories[c.GetString()] = false;
                }

                // On r??cup??re tous les dossards, la cat??gorie associ??e et le nombre de tours du coureur
                nbLines = engine.GetTableSize("dossards");

                for (uint32_t i = 0; i < nbLines; i++)
                {
                    std::vector<Value> dossard;
                    engine.GetTableEntry("dossards", i, dossard);
                    if (dossard.size() == 3)
                    {
                        mDossards[dossard[0].GetInteger()] = dossard[1].GetString();
                        mToursMax[dossard[0].GetInteger()] = dossard[2].GetInteger();
                    }
                }
            }
        }

        uint32_t index = 0;
        for (auto & c : mCategories)
        {
            ImGui::Checkbox(c.first.c_str(), &c.second);

//            if (ImGui::Button( "TOP", ImVec2(80, 30)))
//            {
//                // Top d??part manuel
//            }

            index++;
            if (index < 8)
            {
                ImGui::SameLine();
            }
            else
            {
                index = 0;
                ImGui::NewLine();
            }
        }
    }

    ImGui::NewLine();

    ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | 
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | 
                ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti;

    if (ImGui::BeginTable("table1", 3, tableFlags))
    {
        ImGui::TableSetupColumn("Dossard", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Passages", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Temps", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableHeadersRow();

        for (const auto & e : table)
        {
             ImGui::TableNextRow();

             ImGui::TableSetColumnIndex(0);
             ImGui::Text("%s", std::to_string(e.second.tag).c_str());

             ImGui::TableSetColumnIndex(1);
             ImGui::Text("%s", std::to_string(e.second.laps.size()).c_str());

             ImGui::TableSetColumnIndex(2);

             ImGui::Text("%s", e.second.ToString(startTime).c_str());
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

void TableWindow::ParseAction(const std::vector<Value> &args)
{
    JsonReader reader;
    JsonValue json;

    if (args.size() > 0)
    {
        if (reader.ParseString(json, args[0].GetString()))
        {
            Entry e;
            e.tag = json.FindValue("tag").GetInteger64();
            int64_t time = json.FindValue("time").GetInteger64();

            // on r??cup??re la cat??gorie et le nombre de tours de ce dossard (indiqu?? par le tag) 
            if ((mDossards.count(e.tag) > 0) && (mToursMax.count(e.tag) > 0))
            {
                std::string category = mDossards[e.tag];
                std::uint32_t tours_max = mToursMax[e.tag];
                if (mCategories.count(category) > 0)
                {
                    if (mCategories[category])
                    {
                        mMutex.lock();

                        if (mTable.size() == 0)
                        {
                            mStartTime = time;
                        }

                        // Already detected in the past?
                        if (mTable.count(e.tag) > 0)
                        {
                            std::vector<int64_t> &l = mTable[e.tag].laps;
                            // !!!! IMPORTANT !!!  On a toujours un passage en plus du nombre max de tours ?? effectuer
                            // Ce passage en plus, c'est le d??part !
                            if ((l.size() > 0) && (l.size() <= tours_max))
                            {
                                int64_t diff = time - l[l.size() - 1];

                                if (diff > mWindow)
                                {
                                    mTable[e.tag].laps.push_back(time);
                                }
                            }

                        }
                        else
                        {
                            e.laps.push_back(time);
                            mTable[e.tag] = e;
                        }

                        mMutex.unlock();
                    }
                    else
                    {
                        TLogError("Cat??gorie " + category + " interdite");
                    }
                }
                else
                {
                    TLogError("Cat??gorie inconnue: " + category);
                }
            }
            else
            {
                TLogError("Dossard inconnu: " + std::to_string(e.tag));
            }
        }
    }
}
