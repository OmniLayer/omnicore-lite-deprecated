/**
 * DESIGN DRAFT - Mostly non-functional
 **/


// Copyright (c) 2011-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sendmpdialog.h"
#include "ui_sendmpdialog.h"

#include "omnicore_qtutils.h"

#include "clientmodel.h"
#include "walletmodel.h"

#include "omnicore/createpayload.h"
#include "omnicore/errors.h"
#include "omnicore/omnicore.h"
#include "omnicore/parse_string.h"
#include "omnicore/pending.h"
#include "omnicore/sp.h"
#include "omnicore/tally.h"
#include "omnicore/utilsbitcoin.h"
#include "omnicore/wallettxs.h"

#include "amount.h"
#include "base58.h"
#include "main.h"
#include "sync.h"
#include "uint256.h"
#include "wallet/wallet.h"

#include <stdint.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <QDateTime>
#include <QDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QString>
#include <QWidget>

using std::ostringstream;
using std::string;

using namespace mastercore;

bool txChooserShown = false;

SendMPDialog::SendMPDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SendMPDialog),
    clientModel(0),
    walletModel(0)
{
    ui->setupUi(this);

#ifdef Q_OS_MAC // Icons on push buttons are very uncommon on Mac
    ui->clearButton->setIcon(QIcon());
    ui->sendButton->setIcon(QIcon());
#endif
#if QT_VERSION >= 0x040700 // populate placeholder text
    ui->sendToLineEdit->setPlaceholderText("Enter an Omni Layer Lite address (e.g. LLomniqvhCKdMEQNkezXH71tmrvfxCL5Ju)");
    ui->amountLineEdit->setPlaceholderText("Enter Amount");
#endif

    ui->typeCombo->addItem("Simple Send","0");
    ui->typeCombo->addItem("Send All","4");
    ui->typeCombo->addItem("Issuance (Fixed)","50");
    ui->typeCombo->addItem("Issuance (Crowdsale)","51");
    ui->typeCombo->addItem("Close Crowdsale","53");
    ui->typeCombo->addItem("Issuance (Managed)","54");
    ui->typeCombo->addItem("Grant Tokens","55");
    ui->typeCombo->addItem("Revoke Tokens","56");
    ui->typeCombo->addItem("Change Issuer","70");

    ui->typeCombo->hide();
    ui->typeLabel->setText("Simple Send");

    ui->wgtCrowd->hide();
    ui->wgtProp->hide();
    ui->wgtPropOptions->hide();

    // connect actions
    connect(ui->propertyComboBox, SIGNAL(activated(int)), this, SLOT(propertyComboBoxChanged(int)));
    connect(ui->sendFromComboBox, SIGNAL(activated(int)), this, SLOT(sendFromComboBoxChanged(int)));
    connect(ui->clearButton, SIGNAL(clicked()), this, SLOT(clearButtonClicked()));
    connect(ui->sendButton, SIGNAL(clicked()), this, SLOT(sendButtonClicked()));
    connect(ui->settingsButton, SIGNAL(clicked()), this, SLOT(settingsButtonClicked()));
    connect(ui->typeCombo, SIGNAL(activated(int)), this, SLOT(typeComboBoxChanged(int)));

    // initial update
    balancesUpdated();
}

SendMPDialog::~SendMPDialog()
{
    delete ui;
}

void SendMPDialog::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if (model != NULL) {
        connect(model, SIGNAL(refreshOmniBalance()), this, SLOT(balancesUpdated()));
        connect(model, SIGNAL(reinitOmniState()), this, SLOT(balancesUpdated()));
    }
}

void SendMPDialog::setWalletModel(WalletModel *model)
{
    // use wallet model to get visibility into BTC balance changes for fees
    this->walletModel = model;
    if (model != NULL) {
       connect(model, SIGNAL(balanceChanged(CAmount,CAmount,CAmount,CAmount,CAmount,CAmount)), this, SLOT(updateFrom()));
    }
}

void SendMPDialog::updatePropSelector()
{
    LOCK(cs_tally);

    uint32_t nextPropIdMainEco = GetNextPropertyId(true);  // these allow us to end the for loop at the highest existing
    uint32_t nextPropIdTestEco = GetNextPropertyId(false); // property ID rather than a fixed value like 100000 (optimization)
    QString spId = ui->propertyComboBox->itemData(ui->propertyComboBox->currentIndex()).toString();
    ui->propertyComboBox->clear();

    for (unsigned int propertyId = 1; propertyId < nextPropIdMainEco; propertyId++) {
        if (global_balance_money[propertyId] > 0) {
            std::string spName = getPropertyName(propertyId);
            std::string spId = strprintf("%d", propertyId);
            if(spName.size()>23) spName=spName.substr(0,23) + "...";
            spName += " (#" + spId + ")";
            if (isPropertyDivisible(propertyId)) { spName += " [D]"; } else { spName += " [I]"; }
            ui->propertyComboBox->addItem(spName.c_str(),spId.c_str());
        }
    }
    for (unsigned int propertyId = 2147483647; propertyId < nextPropIdTestEco; propertyId++) {
        if (global_balance_money[propertyId] > 0) {
            std::string spName = getPropertyName(propertyId);
            std::string spId = strprintf("%d", propertyId);
            if(spName.size()>23) spName=spName.substr(0,23)+"...";
            spName += " (#" + spId + ")";
            if (isPropertyDivisible(propertyId)) { spName += " [D]"; } else { spName += " [I]"; }
            ui->propertyComboBox->addItem(spName.c_str(),spId.c_str());
        }
    }
    int propIdx = ui->propertyComboBox->findData(spId);
    if (propIdx != -1) { ui->propertyComboBox->setCurrentIndex(propIdx); }
}

void SendMPDialog::clearFields()
{
    ui->sendToLineEdit->setText("");
    ui->amountLineEdit->setText("");
}

void SendMPDialog::updateFrom()
{
    // check if this from address has sufficient fees for a send, if not light up warning label
    std::string currentSetFromAddress = ui->sendFromComboBox->currentText().toStdString();
    size_t spacer = currentSetFromAddress.find(" ");
    if (spacer!=std::string::npos) {
        currentSetFromAddress = currentSetFromAddress.substr(0,spacer);
        ui->sendFromComboBox->setEditable(true);
        QLineEdit *comboDisplay = ui->sendFromComboBox->lineEdit();
        comboDisplay->setText(QString::fromStdString(currentSetFromAddress));
        comboDisplay->setReadOnly(true);
    }

    if (currentSetFromAddress.empty()) {
        ui->balanceLabel->setText(QString::fromStdString("Address Balance: N/A"));
        ui->feeWarningLabel->setVisible(false);
    } else {
        // update the balance for the selected address
        QString spId = ui->propertyComboBox->itemData(ui->propertyComboBox->currentIndex()).toString();
        uint32_t propertyId = spId.toUInt();
        if (propertyId > 0) {
            ui->balanceLabel->setText(QString::fromStdString("Address Balance: " + FormatMP(propertyId, getUserAvailableMPbalance(currentSetFromAddress, propertyId)) + getTokenLabel(propertyId)));
        }
        // warning label will be lit if insufficient fees for simple send (16 byte payload)
        if (CheckFee(currentSetFromAddress, 16)) {
            ui->feeWarningLabel->setVisible(false);
        } else {
            ui->feeWarningLabel->setText("WARNING: The sending address is low on BTC for transaction fees. Please topup the BTC balance for the sending address to send Omni Layer transactions.");
            ui->feeWarningLabel->setVisible(true);
        }
    }
}

void SendMPDialog::updateProperty()
{
    // cache currently selected from address & clear address selector
    std::string currentSetFromAddress = ui->sendFromComboBox->currentText().toStdString();
    ui->sendFromComboBox->clear();

    // populate from address selector
    QString spId = ui->propertyComboBox->itemData(ui->propertyComboBox->currentIndex()).toString();
    uint32_t propertyId = spId.toUInt();
    LOCK(cs_tally);
    for (std::unordered_map<string, CMPTally>::iterator my_it = mp_tally_map.begin(); my_it != mp_tally_map.end(); ++my_it) {
        string address = (my_it->first).c_str();
        uint32_t id = 0;
        bool includeAddress=false;
        (my_it->second).init();
        while (0 != (id = (my_it->second).next())) {
            if(id == propertyId) { includeAddress=true; break; }
        }
        if (!includeAddress) continue; //ignore this address, has never transacted in this propertyId
        if (IsMyAddress(address) != ISMINE_SPENDABLE) continue; // ignore this address, it's not spendable
        if (!getUserAvailableMPbalance(address, propertyId)) continue; // ignore this address, has no available balance to spend
        ui->sendFromComboBox->addItem(QString::fromStdString(address + " \t" + FormatMP(propertyId, getUserAvailableMPbalance(address, propertyId)) + getTokenLabel(propertyId)));
    }

    // attempt to set from address back to cached value
    int fromIdx = ui->sendFromComboBox->findText(QString::fromStdString(currentSetFromAddress), Qt::MatchContains);
    if (fromIdx != -1) { ui->sendFromComboBox->setCurrentIndex(fromIdx); } // -1 means the cached from address doesn't have a balance in the newly selected property

    // populate balance for global wallet
    ui->globalBalanceLabel->setText(QString::fromStdString("Wallet Balance (Available): " + FormatMP(propertyId, global_balance_money[propertyId])));

#if QT_VERSION >= 0x040700
    // update placeholder text
    if (isPropertyDivisible(propertyId)) { ui->amountLineEdit->setPlaceholderText("Enter Divisible Amount"); } else { ui->amountLineEdit->setPlaceholderText("Enter Indivisible Amount"); }
#endif
}

void SendMPDialog::sendMPTransaction()
{
    // obtain the type of tx we are sending
    QString typeIdStr = ui->typeCombo->itemData(ui->typeCombo->currentIndex()).toString();
    if (typeIdStr.toStdString().empty()) return;
    uint32_t typeId = typeIdStr.toUInt();

    // obtain the selected sender & reference addresses
    string strFromAddress = ui->sendFromComboBox->currentText().toStdString();
    CBitcoinAddress fromAddress;
    string strRefAddress = ui->sendToLineEdit->text().toStdString();
    CBitcoinAddress refAddress;
    if (false == strFromAddress.empty()) fromAddress.SetString(strFromAddress);
    if (!fromAddress.IsValid()) {
        QMessageBox::critical( this, "Unable to send transaction",
        "The sender address selected is not valid.\n\nPlease double-check the transction details thoroughly before retrying your send transaction." );
        return;
    }
    if (typeId == MSC_TYPE_SIMPLE_SEND || typeId == MSC_TYPE_SEND_ALL || typeId == MSC_TYPE_GRANT_PROPERTY_TOKENS) {
        // obtain the entered recipient address
        if (false == strRefAddress.empty()) refAddress.SetString(strRefAddress);
        if (!refAddress.IsValid()) {
            QMessageBox::critical( this, "Unable to send transaction",
            "The recipient address entered is not valid.\n\nPlease double-check the transction details thoroughly before retrying your send transaction." );
            return;
        }
    }

    // obtain the property being sent
    uint32_t propertyId = 0;
    if (typeId == MSC_TYPE_SIMPLE_SEND || typeId == MSC_TYPE_CREATE_PROPERTY_VARIABLE || typeId == MSC_TYPE_CLOSE_CROWDSALE ||
        typeId == MSC_TYPE_GRANT_PROPERTY_TOKENS || typeId == MSC_TYPE_REVOKE_PROPERTY_TOKENS || typeId == MSC_TYPE_CHANGE_ISSUER_ADDRESS) {
        QString spId = ui->propertyComboBox->itemData(ui->propertyComboBox->currentIndex()).toString();
        if (spId.toStdString().empty()) {
            QMessageBox::critical( this, "Unable to send transaction",
            "The property selected is not valid.\n\nPlease double-check the transction details thoroughly before retrying your send transaction." );
            return;
        }
        propertyId = spId.toUInt();
    }

    // obtain the metadata
    std::string strPropName = ui->nameLE->text().toStdString();
    std::string strPropURL = ui->urlLE->text().toStdString();

    // obtain the ecosystem
    bool testEco = false;
    if (typeId == MSC_TYPE_CREATE_PROPERTY_FIXED || typeId == MSC_TYPE_CREATE_PROPERTY_VARIABLE ||
        typeId == MSC_TYPE_CREATE_PROPERTY_MANUAL || typeId == MSC_TYPE_SEND_ALL) {
        if (ui->chkTestEco->isChecked()) testEco = true;
    }

    // obtain the divisibility
    bool divisible = false;
    if (typeId == MSC_TYPE_CREATE_PROPERTY_FIXED || typeId == MSC_TYPE_CREATE_PROPERTY_VARIABLE || typeId == MSC_TYPE_CREATE_PROPERTY_MANUAL) {
        if (ui->chkDivisible->isChecked()) divisible = true;
    } else {
        divisible = isPropertyDivisible(propertyId);
    }

    // obtain the amount
    // warn if we have to truncate the amount due to a decimal amount for an indivisible property, but allow to continue
    int64_t sendAmount = 0;
    string strAmount = ui->amountLineEdit->text().toStdString();
    if (typeId == MSC_TYPE_SIMPLE_SEND || typeId == MSC_TYPE_CREATE_PROPERTY_FIXED || typeId == MSC_TYPE_CREATE_PROPERTY_VARIABLE ||
        typeId == MSC_TYPE_GRANT_PROPERTY_TOKENS || typeId == MSC_TYPE_REVOKE_PROPERTY_TOKENS) {
        if (!divisible) {
            size_t pos = strAmount.find(".");
            if (pos!=std::string::npos) {
                string tmpStrAmount = strAmount.substr(0,pos);
                string strMsgText = "The amount entered contains a decimal however the property being transacted is indivisible.\n\nThe amount entered will be truncated as follows:\n";
                strMsgText += "Original amount entered: " + strAmount + "\nAmount that will be transacted: " + tmpStrAmount + "\n\n";
                strMsgText += "Do you still wish to proceed with the transaction?";
                QString msgText = QString::fromStdString(strMsgText);
                QMessageBox::StandardButton responseClick;
                responseClick = QMessageBox::question(this, "Amount truncation warning", msgText, QMessageBox::Yes|QMessageBox::No);
                if (responseClick == QMessageBox::No) {
                    QMessageBox::critical( this, "Transaction cancelled",
                    "The transaction has been cancelled.\n\nPlease double-check the transction details thoroughly before retrying your send transaction." );
                    return;
                }
                strAmount = tmpStrAmount;
                ui->amountLineEdit->setText(QString::fromStdString(strAmount));
            }
        }
        // use strToInt64 function to get the amount, using divisibility of the property
        sendAmount = StrToInt64(strAmount, divisible);
        if (0>=sendAmount) {
            QMessageBox::critical( this, "Unable to send transaction",
            "The amount entered is not valid.\n\nPlease double-check the transction details thoroughly before retrying your send transaction." );
            return;
        }
    }

    // check if wallet is still syncing, as this will currently cause a lockup if we try to send - compare our chain to peers to see if we're up to date
    // Bitcoin Core devs have removed GetNumBlocksOfPeers, switching to a time based best guess scenario
    uint32_t intBlockDate = GetLatestBlockTime();  // uint32, not using time_t for portability
    QDateTime currentDate = QDateTime::currentDateTime();
    int secs = QDateTime::fromTime_t(intBlockDate).secsTo(currentDate);
    if(secs > 90*60) {
        QMessageBox::critical( this, "Unable to send transaction",
        "The client is still synchronizing.  Sending transactions can currently be performed only when the client has completed synchronizing." );
        return;
    }

    // ##### PER TX CHECKS & PAYLOAD CREATION #####
    std::vector<unsigned char> payload;

        // == Simple Send
        if (typeId == MSC_TYPE_SIMPLE_SEND) {
            // check if sending address has enough funds
            int64_t balanceAvailable = getUserAvailableMPbalance(fromAddress.ToString(), propertyId); //getMPbalance(fromAddress.ToString(), propertyId, MONEY);
            if (sendAmount>balanceAvailable) {
                QMessageBox::critical( this, "Unable to send transaction",
                "The selected sending address does not have a sufficient balance to cover the amount entered.\n\nPlease double-check the transction details thoroughly before retrying your send transaction." );
                return;
            }
            // validation checks all look ok, let's throw up a confirmation dialog
            string strMsgText = "You are about to send the following transaction, please check the details thoroughly:\n\n";
            string propDetails = getPropertyName(propertyId).c_str();
            string spNum = strprintf("%d", propertyId);
            propDetails += " (#" + spNum + ")";
            strMsgText += "From: " + fromAddress.ToString() + "\nTo: " + refAddress.ToString() + "\nProperty: " + propDetails + "\nAmount that will be sent: ";
            if (divisible) { strMsgText += FormatDivisibleMP(sendAmount); } else { strMsgText += FormatIndivisibleMP(sendAmount); }
            strMsgText += "\n\nAre you sure you wish to send this transaction?";
            QString msgText = QString::fromStdString(strMsgText);
            QMessageBox::StandardButton responseClick;
            responseClick = QMessageBox::question(this, "Confirm send transaction", msgText, QMessageBox::Yes|QMessageBox::No);
            if (responseClick == QMessageBox::No) {
                QMessageBox::critical( this, "Send transaction cancelled",
                "The send transaction has been cancelled.\n\nPlease double-check the transction details thoroughly before retrying your send transaction." );
                return;
            }
            // create a payload for the transaction
            payload = CreatePayload_SimpleSend(propertyId, sendAmount);
        }
        // == Create Fixed
        if (typeId == MSC_TYPE_CREATE_PROPERTY_FIXED) {
            uint8_t ecosystem = 1;
            std::string strEco = "Main Ecosystem";
            if (testEco) {
                ecosystem = 2;
                strEco = "Test Ecosystem";
            }
            uint16_t propertyType = 1;
            std::string strPropType = "Indivisible";
            if (divisible) {
                propertyType = 2;
                strPropType = "Divisible";
            }

            string strMsgText = "You are about to send the following transaction, please check the details thoroughly:\n\n";
            strMsgText += "Type: Create Property (Fixed)\n\n";
            strMsgText += "From: " + fromAddress.ToString() + "\n";
            strMsgText += "Ecosystem:   " + strEco + "\n";
            strMsgText += "Property Type:   " + strPropType + "\n";
            strMsgText += "Property Name:   " + strPropName + "\n";
            strMsgText += "Property URL::   " + strPropURL + "\n";
            strMsgText += "Amout of Tokens:   " + strAmount + "\n";

            strMsgText += "\nAre you sure you wish to send this transaction?";
            QString msgText = QString::fromStdString(strMsgText);
            QMessageBox::StandardButton responseClick;
            responseClick = QMessageBox::question(this, "Confirm transaction", msgText, QMessageBox::Yes|QMessageBox::No);
            if (responseClick == QMessageBox::No) {
                QMessageBox::critical( this, "Transaction cancelled",
                "The transaction has been cancelled.\n\nPlease double-check the transction details thoroughly before retrying your transaction." );
                return;
            }
            // create a payload for the transaction
            payload = CreatePayload_IssuanceFixed(ecosystem, propertyType, 0, strPropName, strPropURL, "", sendAmount);
        }

    // ##### END PER TX CHECKS & PAYLOAD CREATION #####

    // unlock the wallet
    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if(!ctx.isValid()) {
        QMessageBox::critical( this, "Send transaction failed",
        "The send transaction has been cancelled.\n\nThe wallet unlock process must be completed to send a transaction." );
        return; // unlock wallet was cancelled/failed
    }

    // request the wallet build the transaction (and if needed commit it) - note UI does not support added reference amounts currently
    uint256 txid;
    std::string rawHex;
    int result = -1;
    if (typeId == MSC_TYPE_SIMPLE_SEND || typeId == MSC_TYPE_SEND_ALL || typeId == MSC_TYPE_GRANT_PROPERTY_TOKENS) {
        result = WalletTxBuilder(fromAddress.ToString(), refAddress.ToString(), 0, payload, txid, rawHex, autoCommit);
    } else {
        result = WalletTxBuilder(fromAddress.ToString(), "", 0, payload, txid, rawHex, autoCommit);
    }

    // check error and return the txid (or raw hex depending on autocommit)
    if (result != 0) {
        QMessageBox::critical( this, "Send transaction failed",
        "The send transaction has failed.\n\nThe error code was: " + QString::number(result) + "\nThe error message was:\n" + QString::fromStdString(error_str(result)));
        return;
    } else {
        if (!autoCommit) {
            PopulateSimpleDialog(rawHex, "Raw Hex (auto commit is disabled)", "Raw transaction hex");
        } else {
            PendingAdd(txid, fromAddress.ToString(), MSC_TYPE_SIMPLE_SEND, propertyId, sendAmount);
            PopulateTXSentDialog(txid.GetHex());
        }
    }
    clearFields();






}

void SendMPDialog::sendFromComboBoxChanged(int idx)
{
    updateFrom();
}

void SendMPDialog::propertyComboBoxChanged(int idx)
{
    updateProperty();
    updateFrom();
}

void SendMPDialog::typeComboBoxChanged(int idx)
{
    QString typeIdStr = ui->typeCombo->itemData(ui->typeCombo->currentIndex()).toString();
    if (typeIdStr.toStdString().empty()) return;
    uint32_t typeId = typeIdStr.toUInt();

    QString typeStr = ui->typeCombo->itemText(ui->typeCombo->currentIndex());
    ui->typeLabel->setText(typeStr);

    switch (typeId)
    {
        case MSC_TYPE_SIMPLE_SEND:
            ui->amountLineEdit->show();
            ui->amountLabel->setText("Amount");
            ui->wgtCrowd->hide();
            ui->wgtProp->hide();
            ui->wgtPropOptions->hide();
            ui->wgtFrom->show();
            ui->wgtTo->show();
            ui->wgtAmount->show();
            ui->balanceLabel->show();
            ui->propertyComboBox->show();
            break;
        case MSC_TYPE_SEND_ALL:
            ui->wgtCrowd->hide();
            ui->wgtProp->hide();
            ui->wgtPropOptions->show();
            ui->wgtAmount->hide();
            ui->balanceLabel->hide();
            ui->wgtFrom->show();
            ui->wgtTo->show();
            ui->chkDivisible->hide();
            break;
        case MSC_TYPE_CREATE_PROPERTY_FIXED:
            ui->amountLineEdit->show();
            ui->amountLabel->setText("Amount");
            ui->wgtCrowd->hide();
            ui->wgtProp->show();
            ui->wgtPropOptions->show();
            ui->wgtAmount->show();
            ui->balanceLabel->hide();
            ui->wgtFrom->show();
            ui->wgtTo->hide();
            ui->propertyComboBox->hide();
            ui->chkDivisible->show();
            break;
        case MSC_TYPE_CREATE_PROPERTY_VARIABLE:
            ui->wgtCrowd->show();
            ui->wgtProp->show();
            ui->wgtPropOptions->show();
            ui->wgtAmount->show();
            ui->amountLabel->setText("Amount");
            ui->balanceLabel->hide();
            ui->wgtFrom->show();
            ui->wgtTo->hide();
            ui->propertyComboBox->show();
            ui->chkDivisible->show();
            break;
        case MSC_TYPE_CLOSE_CROWDSALE:
            ui->wgtCrowd->hide();
            ui->wgtProp->hide();
            ui->wgtPropOptions->hide();
            ui->wgtAmount->show();
            ui->amountLineEdit->hide();
            ui->amountLabel->setText("Property");
            ui->balanceLabel->show();
            ui->wgtFrom->show();
            ui->wgtTo->hide();
            ui->propertyComboBox->show();
            break;
        case MSC_TYPE_CREATE_PROPERTY_MANUAL:
            ui->wgtCrowd->hide();
            ui->wgtProp->show();
            ui->wgtPropOptions->show();
            ui->wgtAmount->hide();
            ui->balanceLabel->hide();
            ui->wgtFrom->show();
            ui->wgtTo->hide();
            ui->propertyComboBox->hide();
            ui->chkDivisible->show();
            break;
        case MSC_TYPE_GRANT_PROPERTY_TOKENS:
            ui->amountLineEdit->show();
            ui->amountLabel->setText("Amount");
            ui->wgtCrowd->hide();
            ui->wgtProp->hide();
            ui->wgtPropOptions->hide();
            ui->wgtAmount->show();
            ui->balanceLabel->show();
            ui->wgtFrom->show();
            ui->wgtTo->show();
            ui->propertyComboBox->show();
            break;
        case MSC_TYPE_REVOKE_PROPERTY_TOKENS:
            ui->amountLineEdit->show();
            ui->amountLabel->setText("Amount");
            ui->wgtCrowd->hide();
            ui->wgtProp->hide();
            ui->wgtPropOptions->hide();
            ui->wgtAmount->show();
            ui->balanceLabel->show();
            ui->wgtFrom->show();
            ui->wgtTo->show();
            ui->propertyComboBox->show();
            break;
        case MSC_TYPE_CHANGE_ISSUER_ADDRESS:
            ui->wgtCrowd->hide();
            ui->wgtProp->hide();
            ui->wgtPropOptions->hide();
            ui->wgtAmount->show();
            ui->amountLineEdit->hide();
            ui->amountLabel->setText("Property");
            ui->balanceLabel->show();
            ui->wgtFrom->show();
            ui->wgtTo->show();
            ui->propertyComboBox->show();
            break;
        default:
            ui->wgtCrowd->hide();
            ui->wgtProp->hide();
            ui->wgtPropOptions->hide();
            ui->wgtAmount->hide();
            ui->balanceLabel->hide();
            ui->wgtFrom->hide();
            ui->wgtTo->hide();
            ui->propertyComboBox->hide();
            break;
    }

}

void SendMPDialog::clearButtonClicked()
{
    clearFields();
}

void SendMPDialog::sendButtonClicked()
{
    sendMPTransaction();
}

void SendMPDialog::settingsButtonClicked()
{
    if (!txChooserShown) {
        ui->typeCombo->show();
        ui->typeLabel->hide();
        txChooserShown = true;
    } else {
        ui->typeCombo->hide();
        ui->typeLabel->show();
        txChooserShown = false;
    }
}

void SendMPDialog::balancesUpdated()
{
    updatePropSelector();
    updateProperty();
    updateFrom();
}
