/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "sch_ollama_agent_pane.h"
#include <sch_edit_frame.h>
#include <tools/sch_ollama_agent_tool.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/richtext/richtextctrl.h>
#include <wx/string.h>
#include <wx/button.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/dcbuffer.h>
#include <wx/thread.h>
#include <wx/app.h>
#include <widgets/ui_common.h>
#include <wx/stc/stc.h>
#include <wx/timer.h>
#include <scintilla_tricks.h>
#include <algorithm>
#include <thread>
#include <mutex>
#include <wx/tokenzr.h>
#include <vector>

namespace
{
const wxColour CURSOR_BG( 15, 17, 22 );
const wxColour CURSOR_SURFACE( 26, 28, 36 );
const wxColour CURSOR_HEADER( 20, 22, 30 );
const wxColour CURSOR_BORDER( 48, 52, 63 );
const wxColour CURSOR_MUTED( 150, 152, 168 );
const wxColour CURSOR_PRIMARY( 228, 230, 238 );
const wxColour CURSOR_AGENT_BUBBLE( 34, 36, 45 );
const wxColour CURSOR_AGENT_BORDER( 50, 54, 66 );
const wxColour CURSOR_AGENT_TEXT( 220, 222, 233 );
const wxColour CURSOR_USER_BUBBLE( 26, 26, 26 );
const wxColour CURSOR_USER_BORDER( 40, 44, 56 );
const wxColour CURSOR_THINK_BUBBLE( 26, 28, 36 );
const wxColour CURSOR_THINK_BORDER( 40, 44, 56 );
const wxColour CURSOR_ACCENT( 124, 101, 255 );
const wxColour CURSOR_SUCCESS( 79, 224, 182 );
const wxColour CURSOR_DANGER( 233, 97, 74 );
constexpr int STREAM_UPDATE_INTERVAL_MS = 8;

wxString NormalizeMarkdownLine( const wxString& aLine, bool& aWasHeading )
{
    wxString line = aLine;
    wxString trimmed = line;
    trimmed.Trim( true );

    int headingLevel = 0;
    size_t idx = 0;

    while( idx < line.length() && line[idx] == '#' && headingLevel < 6 )
    {
        ++headingLevel;
        ++idx;
    }

    if( headingLevel > 0 && idx < line.length() && line[idx] == ' ' )
    {
        wxString headingText = line.Mid( idx + 1 );
        headingText.Trim( true );
        headingText.Trim( false );
        headingText.MakeUpper();
        aWasHeading = true;
        return headingText;
    }

    aWasHeading = false;

    line.Replace( wxS( "`" ), wxEmptyString );
    line.Replace( wxS( "**" ), wxEmptyString );
    line.Replace( wxS( "*" ), wxEmptyString );

    return line;
}

wxString TrimBoth( const wxString& aValue )
{
    wxString trimmed = aValue;
    trimmed.Trim( true );
    trimmed.Trim( false );
    return trimmed;
}

static bool IsTableLine( const wxString& aLine )
{
    wxString trimmed = TrimBoth( aLine );

    if( trimmed.IsEmpty() )
        return false;

    int pipeCount = 0;
    for( wxChar ch : trimmed )
    {
        if( ch == '|' )
            ++pipeCount;
    }

    return pipeCount >= 2;
}

static std::vector<wxString> ParseTableRow( const wxString& aLine )
{
    std::vector<wxString> cells;
    wxStringTokenizer tokenizer( aLine, wxS( "|" ), wxTOKEN_RET_EMPTY_ALL );

    while( tokenizer.HasMoreTokens() )
    {
        wxString cell = tokenizer.GetNextToken();
        cell.Trim( true );
        cell.Trim( false );
        cells.push_back( cell );
    }

    while( !cells.empty() && cells.front().IsEmpty() )
        cells.erase( cells.begin() );
    while( !cells.empty() && cells.back().IsEmpty() )
        cells.pop_back();

    return cells;
}

static bool IsTableSeparatorRow( const std::vector<wxString>& aCells )
{
    if( aCells.empty() )
        return false;

    for( const wxString& cell : aCells )
    {
        wxString stripped = TrimBoth( cell );
        if( stripped.IsEmpty() )
            continue;

        for( wxChar ch : stripped )
        {
            if( ch != '-' && ch != ':' )
                return false;
        }
    }

    return true;
}

static wxString RenderTableBlock( const std::vector<std::vector<wxString>>& aRows )
{
    if( aRows.empty() )
        return wxEmptyString;

    std::vector<std::vector<wxString>> rows;
    rows.reserve( aRows.size() );

    bool hadSeparator = false;

    for( const std::vector<wxString>& row : aRows )
    {
        if( IsTableSeparatorRow( row ) )
        {
            hadSeparator = true;
            continue;
        }

        rows.push_back( row );
    }

    if( rows.empty() )
        return wxEmptyString;

    size_t columnCount = 0;
    for( const auto& row : rows )
        columnCount = std::max( columnCount, row.size() );

    if( columnCount == 0 )
        return wxEmptyString;

    std::vector<size_t> colWidths( columnCount, 0 );

     for( const auto& row : rows )
     {
         for( size_t idx = 0; idx < columnCount; ++idx )
         {
             wxString cell = ( idx < row.size() ) ? row[idx] : wxString( wxEmptyString );
             colWidths[idx] = std::max( colWidths[idx], static_cast<size_t>( cell.length() ) );
         }
     }

     auto formatRow = [&]( const std::vector<wxString>& aRow )
     {
         wxString line;
         for( size_t idx = 0; idx < columnCount; ++idx )
         {
             wxString cell = ( idx < aRow.size() ) ? aRow[idx] : wxString( wxEmptyString );
            wxString padded = cell;
            size_t pad = ( idx < colWidths.size() ) ? ( colWidths[idx] - cell.length() ) : 0;
            line << wxS( " " ) << padded;
            line << wxString( ' ', static_cast<size_t>( pad ) + 2 );

            if( idx + 1 < columnCount )
                line << wxS( "|" );
        }

        return line.Trim( false );
    };

    wxString result;

    for( size_t rowIdx = 0; rowIdx < rows.size(); ++rowIdx )
    {
        result << formatRow( rows[rowIdx] ) << wxS( "\n" );

        if( rowIdx == 0 && hadSeparator )
        {
            wxString separatorLine;
            for( size_t idx = 0; idx < columnCount; ++idx )
            {
                size_t width = idx < colWidths.size() ? colWidths[idx] : 0;
                separatorLine << wxString( '-', width + 2 );
                if( idx + 1 < columnCount )
                    separatorLine << wxS( "+" );
            }

            result << separatorLine << wxS( "\n" );
        }
    }

    return result;
}

static wxString FormatMarkdownTables( const wxString& aMessage )
{
    if( aMessage.empty() )
        return aMessage;

    wxStringTokenizer tokenizer( aMessage, wxS( "\n" ), wxTOKEN_RET_EMPTY_ALL );
    std::vector<wxString> lines;

    while( tokenizer.HasMoreTokens() )
        lines.push_back( tokenizer.GetNextToken() );

    bool endsWithNewline = !aMessage.empty() && aMessage.Last() == '\n';

    wxString result;
    std::vector<std::vector<wxString>> tableRows;

    auto flushTable = [&]()
    {
        if( tableRows.empty() )
            return;

        result << RenderTableBlock( tableRows );
        tableRows.clear();
    };

    for( size_t idx = 0; idx < lines.size(); ++idx )
    {
        wxString& line = lines[idx];

        if( IsTableLine( line ) )
        {
            tableRows.push_back( ParseTableRow( line ) );
            continue;
        }

        flushTable();
        result << line;

        if( idx + 1 < lines.size() || endsWithNewline )
            result << wxS( "\n" );
    }

    flushTable();

    return result;
}
}

enum class REQUEST_FAILURE_REASON
{
    GENERIC = 0,
    AGENT_UNAVAILABLE = 1
};

// Message bubble panel for chat messages with Cursor-inspired styling.
class MESSAGE_BUBBLE : public wxPanel
{
public:
    MESSAGE_BUBBLE( wxWindow* aParent, const wxString& aMessage, CHAT_BUBBLE_KIND aKind ) :
        wxPanel( aParent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE ),
        m_textCtrl( nullptr ),
        m_kind( aKind ),
        m_cornerRadius( 12 ),
        m_padding( 12 ),
        m_maxWidth( 520 )
    {
        configureAppearance();

        SetBackgroundColour( CURSOR_BG );
        SetBackgroundStyle( wxBG_STYLE_PAINT );
        SetDoubleBuffered( true );

        wxBoxSizer* mainSizer = new wxBoxSizer( wxHORIZONTAL );

        if( m_kind == CHAT_BUBBLE_KIND::USER )
            mainSizer->AddStretchSpacer();

        m_contentSizer = new wxBoxSizer( wxVERTICAL );

        wxString initialText = aMessage;
        if( initialText.IsEmpty() && m_kind == CHAT_BUBBLE_KIND::THINKING )
            initialText = _( "Thinking..." );

        m_textCtrl = new wxRichTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                         wxDefaultSize,
                                         wxBORDER_NONE | wxRE_MULTILINE );
        m_textCtrl->SetBackgroundColour( m_bgColor );
        m_textCtrl->SetEditable( false );
        m_textCtrl->ShowScrollbars( wxSHOW_SB_NEVER, wxSHOW_SB_NEVER );
        m_textCtrl->Bind( wxEVT_CHAR, []( wxKeyEvent& ) {} );

        m_contentSizer->Add( m_textCtrl, 1, wxEXPAND | wxALL, m_padding );
        SetFormattedText( initialText );

        mainSizer->Add( m_contentSizer, 0,
                        m_kind == CHAT_BUBBLE_KIND::USER ? ( wxALIGN_RIGHT | wxEXPAND )
                                                         : ( wxALIGN_LEFT | wxEXPAND ),
                        0 );

        if( m_kind != CHAT_BUBBLE_KIND::USER )
            mainSizer->AddStretchSpacer();

        SetSizer( mainSizer );
        Layout();
        Fit();

        Bind( wxEVT_PAINT, &MESSAGE_BUBBLE::OnPaint, this );
    }

    void UpdateText( const wxString& aMessage )
    {
        SetFormattedText( aMessage );
    }

private:
    void configureAppearance()
    {
        switch( m_kind )
        {
        case CHAT_BUBBLE_KIND::USER:
            m_bgColor = CURSOR_USER_BUBBLE;
            m_textColor = *wxWHITE;
            m_borderColor = CURSOR_USER_BORDER;
            break;
        case CHAT_BUBBLE_KIND::THINKING:
            m_bgColor = CURSOR_THINK_BUBBLE;
            m_textColor = CURSOR_MUTED;
            m_borderColor = CURSOR_THINK_BORDER;
            break;
        case CHAT_BUBBLE_KIND::AGENT:
        default:
            m_bgColor = CURSOR_AGENT_BUBBLE;
            m_textColor = CURSOR_AGENT_TEXT;
            m_borderColor = CURSOR_AGENT_BORDER;
            break;
        }
    }

    void OnPaint( wxPaintEvent& )
    {
        wxAutoBufferedPaintDC dc( this );
        wxSize size = GetClientSize();

        dc.SetPen( wxPen( m_borderColor, 1 ) );
        dc.SetBrush( wxBrush( m_bgColor ) );
        dc.DrawRoundedRectangle( 0, 0, size.GetWidth(), size.GetHeight(), m_cornerRadius );
    }

    void SetFormattedText( const wxString& aMessage )
    {
        if( !m_textCtrl )
            return;

        wxFont baseFont = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
        baseFont.SetPointSize( 11 );
        baseFont.SetStyle( wxFONTSTYLE_NORMAL );
        baseFont.SetWeight( wxFONTWEIGHT_NORMAL );
        baseFont.SetFamily( wxFONTFAMILY_DEFAULT );

        wxTextAttr defaultAttr( m_textColor, m_bgColor, baseFont );
        wxColour codeBg = m_bgColor;
        codeBg = codeBg.ChangeLightness( 110 );
        wxTextAttr codeAttr( m_textColor, codeBg, baseFont );

        bool wasEditable = m_textCtrl->IsEditable();
        m_textCtrl->SetEditable( true );
        m_textCtrl->Freeze();
        m_textCtrl->SetDefaultStyle( defaultAttr );
        m_textCtrl->SetBackgroundColour( m_bgColor );
        m_textCtrl->Clear();

        wxString normalized = FormatMarkdownTables( aMessage );

        bool bold = false;
        bool heading = false;
        bool codeBlock = false;
        bool startOfLine = true;

        for( size_t i = 0; i < normalized.length(); )
        {
            if( normalized[i] == '\r' )
            {
                ++i;
                continue;
            }

            if( normalized.Mid( i, 3 ) == wxS( "```" ) )
            {
                if( codeBlock )
                {
                    m_textCtrl->EndStyle();
                    codeBlock = false;
                    i += 3;
                    startOfLine = true;
                    continue;
                }
                else
                {
                    size_t closing = normalized.find( wxS( "```" ), i + 3 );
                    if( closing == wxString::npos )
                    {
                        m_textCtrl->WriteText( wxS( "```" ) );
                        i += 3;
                        continue;
                    }

                    m_textCtrl->BeginStyle( codeAttr );
                    codeBlock = true;
                    i += 3;
                    startOfLine = true;
                    continue;
                }
            }

            if( normalized[i] == '\n' )
            {
                if( heading )
                {
                    m_textCtrl->EndStyle();
                    heading = false;
                }

                m_textCtrl->Newline();
                startOfLine = true;
                ++i;
                continue;
            }

            if( !codeBlock && normalized.Mid( i, 2 ) == wxS( "**" ) )
            {
                size_t closing = normalized.find( wxS( "**" ), i + 2 );
                if( closing == wxString::npos )
                {
                    m_textCtrl->WriteText( wxS( "**" ) );
                    i += 2;
                    continue;
                }

                if( bold )
                    m_textCtrl->EndBold();
                else
                    m_textCtrl->BeginBold();

                bold = !bold;
                i += 2;
                startOfLine = false;
                continue;
            }

            if( !codeBlock && startOfLine )
            {
                if( normalized.Mid( i, 2 ) == wxS( "- " ) || normalized.Mid( i, 2 ) == wxS( "* " ) )
                {
                    m_textCtrl->WriteText( wxS( "• " ) );
                    i += 2;
                    startOfLine = false;
                    continue;
                }

                if( normalized[i] == '#' )
                {
                    size_t pos = i;
                    int level = 0;

                    while( pos < normalized.length() && normalized[pos] == '#' && level < 6 )
                    {
                        ++level;
                        ++pos;
                    }

                    if( pos < normalized.length() && normalized[pos] == ' ' )
                    {
                        i = pos + 1;
                        wxFont headingFont( baseFont );
                        headingFont.SetPointSize( baseFont.GetPointSize() + std::max( 0, 4 - level ) );
                        headingFont.SetWeight( wxFONTWEIGHT_BOLD );
                        wxTextAttr headingAttr( m_textColor, m_bgColor, headingFont );
                        m_textCtrl->BeginStyle( headingAttr );
                        heading = true;
                        startOfLine = false;
                        continue;
                    }
                }
            }

            wxChar ch = normalized[i];
            wxString charStr( ch );
            m_textCtrl->WriteText( charStr );

            if( ch != ' ' && ch != '\t' )
                startOfLine = false;

            ++i;
        }

        if( heading )
            m_textCtrl->EndStyle();
        if( codeBlock )
            m_textCtrl->EndStyle();
        if( bold )
            m_textCtrl->EndBold();

        m_textCtrl->ShowPosition( m_textCtrl->GetLastPosition() );
        m_textCtrl->Thaw();
        m_textCtrl->SetEditable( wasEditable );

        UpdateTextControl();

        if( wxWindow* parent = GetParent() )
            parent->Layout();

        Refresh();
    }

    void UpdateTextControl()
    {
        if( !m_textCtrl )
            return;

        wxString value = m_textCtrl->GetValue();
        wxStringTokenizer tokenizer( value, wxS( "\n" ), wxTOKEN_RET_EMPTY_ALL );
        std::vector<wxString> lines;

        while( tokenizer.HasMoreTokens() )
            lines.push_back( tokenizer.GetNextToken() );

        if( lines.empty() )
            lines.emplace_back( wxEmptyString );

        wxClientDC dc( m_textCtrl );
        dc.SetFont( m_textCtrl->GetFont() );

        int measuredWidth = 0;
        for( const wxString& line : lines )
        {
            wxSize extent = dc.GetTextExtent( line );
            measuredWidth = std::max( measuredWidth, extent.GetWidth() );
        }

        wxWindow* parent = GetParent();
        int parentWidth = parent ? parent->GetClientSize().GetWidth() : m_maxWidth;
        int availableWidth = parentWidth > 0 ? parentWidth - FromDIP( 80 ) : m_maxWidth;
        int minWidth = FromDIP( 220 );

        if( availableWidth <= 0 )
            availableWidth = m_maxWidth;

        int targetWidth = std::clamp( measuredWidth + ( m_padding * 2 ), minWidth,
                                      std::min( m_maxWidth, availableWidth ) );

        int charHeight = m_textCtrl->GetCharHeight();
        if( charHeight <= 0 )
            charHeight = FromDIP( 18 );

        int targetHeight = charHeight * static_cast<int>( lines.size() ) + ( m_padding * 2 );

        m_textCtrl->SetMinSize( wxSize( targetWidth, targetHeight ) );
        m_textCtrl->SetMaxSize( wxSize( targetWidth, targetHeight ) );
        m_contentSizer->Fit( this );
        m_contentSizer->Layout();
        Layout();
        Fit();
        Refresh();
    }

    wxRichTextCtrl* m_textCtrl;
    wxBoxSizer* m_contentSizer;
    wxColour m_bgColor;
    wxColour m_textColor;
    wxColour m_borderColor;
    CHAT_BUBBLE_KIND m_kind;
    int m_cornerRadius;
    int m_padding;
    int m_maxWidth;
};

class TOOL_CALL_BUBBLE : public wxPanel
{
public:
    TOOL_CALL_BUBBLE( wxWindow* aParent, const wxString& aToolName, const wxString& aPayload ) :
        wxPanel( aParent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE ),
        m_statusLabel( nullptr )
    {
        SetBackgroundColour( CURSOR_SURFACE );
        SetForegroundColour( CURSOR_BORDER );
        SetDoubleBuffered( true );

        wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

        wxBoxSizer* headerSizer = new wxBoxSizer( wxHORIZONTAL );
        wxStaticText* title = new wxStaticText( this, wxID_ANY,
                                                wxString::Format( _( "Tool: %s" ), aToolName ) );
        title->SetForegroundColour( CURSOR_PRIMARY );
        wxFont headerFont = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
        headerFont.SetWeight( wxFONTWEIGHT_BOLD );
        headerSizer->Add( title, 0, wxALIGN_CENTER_VERTICAL );

        m_statusLabel = new wxStaticText( this, wxID_ANY, _( "Queued" ) );
        headerSizer->AddStretchSpacer();
        headerSizer->Add( m_statusLabel, 0, wxALIGN_CENTER_VERTICAL );
        mainSizer->Add( headerSizer, 0, wxEXPAND | wxALL, 8 );

        wxStaticText* payloadLabel = new wxStaticText(
                this, wxID_ANY, wxString::Format( _( "Payload: %s" ), aPayload ) );
        payloadLabel->SetForegroundColour( CURSOR_MUTED );
        payloadLabel->Wrap( FromDIP( 260 ) );
        mainSizer->Add( payloadLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8 );

        SetSizer( mainSizer );
        Layout();
    }

    void UpdateStatus( const wxString& aStatus, const wxColour& aColour )
    {
        if( !m_statusLabel )
            return;

        m_statusLabel->SetLabel( aStatus );
        m_statusLabel->SetForegroundColour( aColour );
        Layout();
    }

private:
    wxStaticText* m_statusLabel;
};


SCH_OLLAMA_AGENT_PANE::SCH_OLLAMA_AGENT_PANE( SCH_EDIT_FRAME* aParent ) :
    WX_PANEL( aParent ),
    m_tool( nullptr ),
    m_chatPanel( nullptr ),
    m_chatSizer( nullptr ),
    m_inputCtrl( nullptr ),
    m_scintillaTricks( nullptr ),
    m_sendButton( nullptr ),
    m_clearButton( nullptr ),
    m_cancelButton( nullptr ),
    m_statusText( nullptr ),
    m_isProcessing( false ),
    m_streamingBubble( nullptr ),
    m_streamingText(),
    m_cancelRequested( false ),
    m_reasoningBubble( nullptr ),
    m_inThinkSection( false ),
    m_hasReasoningContent( false ),
    m_reasoningText(),
    m_responseAccumulator(),
    m_streamUpdateTimer( this ),
    m_streamBubbleDirty( false ),
    m_toolCallActive( false )
{
    wxASSERT( dynamic_cast<SCH_EDIT_FRAME*>( aParent ) );

    SetBackgroundColour( CURSOR_BG );
    SetDoubleBuffered( true );

    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    wxPanel* headerPanel = new wxPanel( this, wxID_ANY );
    headerPanel->SetBackgroundColour( CURSOR_HEADER );
    wxBoxSizer* headerSizer = new wxBoxSizer( wxHORIZONTAL );

    wxStaticText* titleText = new wxStaticText( headerPanel, wxID_ANY, _( "Schematic AI Agent" ) );
    wxFont titleFont = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
    titleFont.SetPointSize( 12 );
    titleFont.SetWeight( wxFONTWEIGHT_BOLD );
    titleText->SetFont( titleFont );
    titleText->SetForegroundColour( CURSOR_PRIMARY );
    headerSizer->Add( titleText, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 16 );

    m_statusText = new wxStaticText( headerPanel, wxID_ANY, _( "Checking Python agent..." ) );
    m_statusText->SetForegroundColour( CURSOR_MUTED );
    headerSizer->Add( m_statusText, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12 );

    headerSizer->AddStretchSpacer();

    m_clearButton = new wxButton( headerPanel, wxID_ANY, _( "Clear" ) );
    m_clearButton->SetMinSize( wxSize( 90, 32 ) );
    headerSizer->Add( m_clearButton, 0, wxALL | wxALIGN_CENTER_VERTICAL, 6 );

    headerPanel->SetSizer( headerSizer );
    mainSizer->Add( headerPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6 );

    m_chatPanel = new wxScrolledWindow( this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                       wxVSCROLL | wxBORDER_NONE );
    m_chatPanel->SetBackgroundColour( CURSOR_BG );
    m_chatPanel->SetScrollRate( 0, 15 );
    m_chatPanel->EnableScrolling( false, true );

    m_chatSizer = new wxBoxSizer( wxVERTICAL );
    m_chatPanel->SetSizer( m_chatSizer );
    m_chatSizer->AddSpacer( 8 );

    wxStaticText* welcomeText = new wxStaticText( m_chatPanel, wxID_ANY,
        _( "Cursor-style assistant at your service. Ask me to rework nets, labels, or entire schematics." ) );
    welcomeText->SetForegroundColour( CURSOR_MUTED );
    wxFont welcomeFont = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
    welcomeFont.SetPointSize( 10 );
    welcomeText->SetFont( welcomeFont );
    m_chatSizer->Add( welcomeText, 0, wxALL, 16 );
    m_chatSizer->AddSpacer( 6 );

    mainSizer->Add( m_chatPanel, 1, wxEXPAND | wxLEFT | wxRIGHT, 6 );

    wxPanel* inputPanel = new wxPanel( this, wxID_ANY );
    inputPanel->SetBackgroundColour( CURSOR_BG );
    inputPanel->SetDoubleBuffered( true );
    wxBoxSizer* inputSizer = new wxBoxSizer( wxVERTICAL );

    wxPanel* composerPanel = new wxPanel( inputPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          wxBORDER_SIMPLE );
    composerPanel->SetBackgroundColour( CURSOR_SURFACE );
    composerPanel->SetForegroundColour( CURSOR_BORDER );
    wxBoxSizer* composerSizer = new wxBoxSizer( wxHORIZONTAL );

    // Set up exactly like Text Properties dialog
    m_inputCtrl = new wxStyledTextCtrl( composerPanel, wxID_ANY,
                                        wxDefaultPosition, wxSize( -1, 90 ),
                                        wxBORDER_NONE );
    m_inputCtrl->SetBackgroundColour( CURSOR_SURFACE );
    m_inputCtrl->SetCaretForeground( CURSOR_PRIMARY );
    m_inputCtrl->StyleSetForeground( wxSTC_STYLE_DEFAULT, CURSOR_PRIMARY );
    wxFont inputFont = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
    inputFont.SetPointSize( 11 );
    m_inputCtrl->StyleSetFont( wxSTC_STYLE_DEFAULT, inputFont );
    m_inputCtrl->SetMinSize( wxSize( -1, 90 ) );

    m_inputCtrl->SetEOLMode( wxSTC_EOL_LF );

#ifdef _WIN32
    // Without this setting, on Windows, some esoteric unicode chars create display issue
    // in a wxStyledTextCtrl.
    m_inputCtrl->SetTechnology( wxSTC_TECHNOLOGY_DIRECTWRITE );
#endif

    // Set up SCINTILLA_TRICKS exactly like Text Properties dialog
    m_scintillaTricks = new SCINTILLA_TRICKS( m_inputCtrl, wxT( "" ), false,
            // onAcceptFn - Ctrl/Cmd+Enter or Shift+Enter to send
            [this]( wxKeyEvent& aEvent )
            {
                if( ( aEvent.GetModifiers() == wxMOD_CONTROL || aEvent.GetModifiers() == wxMOD_CMD ) &&
                    ( aEvent.GetKeyCode() == WXK_RETURN || aEvent.GetKeyCode() == WXK_NUMPAD_ENTER ) )
                {
                    sendMessage();
                }
            },
            // onCharFn - no autocomplete needed
            []( wxStyledTextEvent& ) {} );

    // A hack which causes Scintilla to auto-size the text editor canvas
    // See: https://github.com/jacobslusser/ScintillaNET/issues/216
    m_inputCtrl->SetScrollWidth( 1 );
    m_inputCtrl->SetScrollWidthTracking( true );

    KIUI::RegisterHotkeySuppressor( m_inputCtrl );

    m_sendButton = new wxButton( composerPanel, wxID_OK, _( "Send" ) );
    m_sendButton->SetDefault();

    composerSizer->Add( m_inputCtrl, 1, wxEXPAND | wxALL, 12 );
    composerSizer->Add( m_sendButton, 0, wxALIGN_BOTTOM | wxRIGHT | wxTOP | wxBOTTOM, 12 );
    composerPanel->SetSizer( composerSizer );
    inputSizer->Add( composerPanel, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12 );

    wxBoxSizer* footerSizer = new wxBoxSizer( wxHORIZONTAL );

    wxStaticText* helperText = new wxStaticText( inputPanel, wxID_ANY,
        _( "Shift+Enter for newline  |  Esc to cancel streaming" ) );
    helperText->SetForegroundColour( CURSOR_MUTED );
    wxFont helperFont = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
    helperFont.SetPointSize( 9 );
    helperText->SetFont( helperFont );
    footerSizer->Add( helperText, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12 );

    m_cancelButton = new wxButton( inputPanel, wxID_ANY, _( "Stop" ) );
    m_cancelButton->SetMinSize( wxSize( 90, 32 ) );
    m_cancelButton->Enable( false );
    footerSizer->Add( m_cancelButton, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6 );

    inputSizer->Add( footerSizer, 0, wxEXPAND | wxALL, 12 );

    inputPanel->SetSizer( inputSizer );
    mainSizer->Add( inputPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6 );

    SetSizer( mainSizer );
    
    // Event handlers
    Bind( wxEVT_COMMAND_BUTTON_CLICKED, &SCH_OLLAMA_AGENT_PANE::onSendButton, this, wxID_OK );
    m_clearButton->Bind( wxEVT_COMMAND_BUTTON_CLICKED, 
                        [this]( wxCommandEvent& ) { ClearChat(); } );
    m_cancelButton->Bind( wxEVT_COMMAND_BUTTON_CLICKED,
                          [this]( wxCommandEvent& ) { CancelCurrentRequest(); } );
    
    // Bind async response handlers - all use wxCommandEvent/wxEVT_COMMAND_TEXT_UPDATED with different IDs
    Bind( wxEVT_COMMAND_TEXT_UPDATED, &SCH_OLLAMA_AGENT_PANE::OnResponseReceived, this, ID_RESPONSE_RECEIVED );
    Bind( wxEVT_COMMAND_TEXT_UPDATED, &SCH_OLLAMA_AGENT_PANE::OnRequestFailed, this, ID_REQUEST_FAILED );
    Bind( wxEVT_COMMAND_TEXT_UPDATED, &SCH_OLLAMA_AGENT_PANE::OnResponsePartial, this, ID_RESPONSE_PARTIAL );
    Bind( wxEVT_COMMAND_TEXT_UPDATED, &SCH_OLLAMA_AGENT_PANE::OnRequestCancelled, this, ID_REQUEST_CANCELLED );
    Bind( wxEVT_COMMAND_TEXT_UPDATED, &SCH_OLLAMA_AGENT_PANE::OnConnectionCheckResult, this, ID_CONNECTION_CHECK_RESULT );
    
    // Focus on input (exactly like Text Properties dialog)
    m_inputCtrl->SetFocus();

    // Register the entire pane as a hotkey suppressor so any focus within the pane
    // (including buttons, etc.) will suppress editor hotkeys.
    KIUI::RegisterHotkeySuppressor( this, true );

    m_streamUpdateTimer.SetOwner( this );
    Bind( wxEVT_TIMER, &SCH_OLLAMA_AGENT_PANE::OnStreamUpdateTimer, this, m_streamUpdateTimer.GetId() );
}


SCH_OLLAMA_AGENT_PANE::~SCH_OLLAMA_AGENT_PANE()
{
    delete m_scintillaTricks;

    KIUI::UnregisterHotkeySuppressor( this );
    KIUI::UnregisterHotkeySuppressor( m_inputCtrl );

    // Wait for any running thread to finish
    {
        std::lock_guard<std::mutex> lock( m_threadMutex );
        if( m_requestThread.joinable() )
        {
            m_requestThread.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock( m_connectionMutex );
        if( m_connectionThread.joinable() )
        {
            m_connectionThread.join();
        }
    }
}


void SCH_OLLAMA_AGENT_PANE::AddUserMessage( const wxString& aMessage )
{
    if( aMessage.IsEmpty() )
        return;
    
    addMessageToChat( aMessage, CHAT_BUBBLE_KIND::USER );
}


void SCH_OLLAMA_AGENT_PANE::AddAgentMessage( const wxString& aMessage )
{
    if( aMessage.IsEmpty() )
        return;
    
    addMessageToChat( aMessage, CHAT_BUBBLE_KIND::AGENT );
}


wxWindow* SCH_OLLAMA_AGENT_PANE::addMessageToChat( const wxString& aMessage, CHAT_BUBBLE_KIND aKind )
{
    MESSAGE_BUBBLE* bubble = new MESSAGE_BUBBLE( m_chatPanel, aMessage, aKind );

    if( aKind == CHAT_BUBBLE_KIND::THINKING )
        m_reasoningBubble = bubble;
    else if( aKind == CHAT_BUBBLE_KIND::AGENT )
        m_streamingBubble = bubble;

    wxBoxSizer* rowSizer = new wxBoxSizer( wxHORIZONTAL );

    if( aKind == CHAT_BUBBLE_KIND::USER )
        rowSizer->Add( 0, 0, 1, wxEXPAND, 0 );

    rowSizer->Add( bubble, 0, wxALIGN_TOP | wxALL, 10 );

    if( aKind != CHAT_BUBBLE_KIND::USER )
        rowSizer->Add( 0, 0, 1, wxEXPAND, 0 );

    m_chatSizer->Add( rowSizer, 0, wxEXPAND | wxTOP | wxBOTTOM, 5 );
    m_chatSizer->Layout();
    m_chatPanel->SetVirtualSize( m_chatSizer->GetMinSize() );
    m_chatPanel->Layout();
    scrollToBottom();
    m_chatPanel->Refresh();

    return bubble;
}


void SCH_OLLAMA_AGENT_PANE::clearReasoningBubble()
{
    if( m_reasoningBubble )
    {
        m_chatSizer->Detach( m_reasoningBubble );
        m_reasoningBubble->Destroy();
        m_reasoningBubble = nullptr;
        m_chatSizer->Layout();
        m_chatPanel->Layout();
        m_chatPanel->Refresh();
    }

    m_reasoningText.clear();
    m_hasReasoningContent = false;
    m_inThinkSection = false;
}


void SCH_OLLAMA_AGENT_PANE::ClearChat()
{
    m_chatSizer->Clear( true );
    m_streamingBubble = nullptr;
    m_streamingText.clear();
    clearReasoningBubble();
    m_responseAccumulator.clear();
    m_toolCallQueue.clear();
    m_toolCallActive = false;
    m_streamUpdateTimer.Stop();
    m_streamBubbleDirty = false;
    AddAgentMessage( _( "History cleared. Tell me what you want to build next." ) );
}


void SCH_OLLAMA_AGENT_PANE::SetTool( SCH_OLLAMA_AGENT_TOOL* aTool )
{
    m_tool = aTool;
    if( m_tool )
        m_tool->SetToolCallHandler( this );
    StartConnectionCheck();
}


void SCH_OLLAMA_AGENT_PANE::HandleToolCall( const wxString& aToolName, const wxString& aPayload )
{
    queueToolCall( aToolName, aPayload );
}


void SCH_OLLAMA_AGENT_PANE::CancelCurrentRequest()
{
    if( !m_isProcessing || m_cancelRequested.load() )
        return;

    m_cancelRequested.store( true );

    if( m_cancelButton )
        m_cancelButton->Enable( false );

    if( m_statusText )
    {
        m_statusText->SetLabel( _( "Cancelling request..." ) );
        m_statusText->SetForegroundColour( CURSOR_DANGER );
    }

    clearReasoningBubble();
    if( m_streamingBubble )
        m_streamingBubble = nullptr;
    m_streamingText.clear();
    m_responseAccumulator.clear();
    m_toolCallQueue.clear();
    m_toolCallActive = false;
    m_streamUpdateTimer.Stop();
    m_streamBubbleDirty = false;
}


void SCH_OLLAMA_AGENT_PANE::queueToolCall( const wxString& aToolName, const wxString& aPayload )
{
    if( !m_tool )
        return;

    TOOL_CALL_BUBBLE* bubble = new TOOL_CALL_BUBBLE( m_chatPanel, aToolName, aPayload );
    wxBoxSizer* rowSizer = new wxBoxSizer( wxHORIZONTAL );
    rowSizer->Add( bubble, 1, wxEXPAND | wxALL, 10 );
    m_chatSizer->Add( rowSizer, 0, wxEXPAND );
    m_chatSizer->Layout();
    m_chatPanel->SetVirtualSize( m_chatSizer->GetMinSize() );
    m_chatPanel->Layout();
    scrollToBottom();

    m_toolCallQueue.push_back( TOOL_CALL_REQUEST{ aToolName, aPayload, bubble } );
    processNextToolCall();
}


void SCH_OLLAMA_AGENT_PANE::processNextToolCall()
{
    if( m_toolCallActive || m_toolCallQueue.empty() )
        return;

    m_toolCallActive = true;
    TOOL_CALL_REQUEST request = m_toolCallQueue.front();
    m_toolCallQueue.pop_front();

    if( request.bubble && !request.bubble->IsBeingDeleted() )
        request.bubble->UpdateStatus( _( "Running..." ), CURSOR_ACCENT );

    if( !wxTheApp )
    {
        bool success = m_tool ? m_tool->RunToolCommand( request.toolName, request.payload ) : false;
        if( request.bubble && !request.bubble->IsBeingDeleted() )
            request.bubble->UpdateStatus( success ? _( "Completed" ) : _( "Failed" ),
                                          success ? CURSOR_SUCCESS : CURSOR_DANGER );
        m_toolCallActive = false;
        processNextToolCall();
        return;
    }

    wxTheApp->CallAfter( [this, request]()
    {
        bool success = m_tool ? m_tool->RunToolCommand( request.toolName, request.payload ) : false;

        if( request.bubble && !request.bubble->IsBeingDeleted() )
        {
            request.bubble->UpdateStatus( success ? _( "Completed" ) : _( "Failed" ),
                                          success ? CURSOR_SUCCESS : CURSOR_DANGER );
        }

        m_toolCallActive = false;
        processNextToolCall();
    } );
}


void SCH_OLLAMA_AGENT_PANE::StartConnectionCheck()
{
    if( !m_tool )
        return;

    if( m_statusText )
    {
        m_statusText->SetLabel( _( "Checking Python agent connection..." ) );
        m_statusText->SetForegroundColour( CURSOR_MUTED );
    }

    std::lock_guard<std::mutex> lock( m_connectionMutex );
    if( m_connectionThread.joinable() )
        m_connectionThread.join();

    m_connectionThread = std::thread( [this]()
    {
        bool success = false;

        if( m_tool )
        {
            if( OLLAMA_CLIENT* client = m_tool->GetOllama() )
            {
                // Use IsAvailable() for quick connection check (no timeout issues)
                success = client->IsAvailable();
            }
        }

        wxCommandEvent* event = new wxCommandEvent( wxEVT_COMMAND_TEXT_UPDATED, ID_CONNECTION_CHECK_RESULT );
        event->SetInt( success ? 1 : 0 );
        event->SetString( success ? _( "Connected to Python agent" ) : _( "Unable to reach Python agent" ) );
        wxQueueEvent( this, event );
    } );
}


wxString SCH_OLLAMA_AGENT_PANE::filterToolLines( const wxString& aChunk, bool /*aFromStreaming*/ )
{
    if( aChunk.IsEmpty() )
        return wxEmptyString;

    wxString filtered;
    wxStringTokenizer tokenizer( aChunk, wxS( "\n" ), wxTOKEN_RET_EMPTY_ALL );
    bool firstLine = true;

    while( tokenizer.HasMoreTokens() )
    {
        wxString line = tokenizer.GetNextToken();
        wxString trimmed = line;
        trimmed.Trim( true ).Trim( false );

        if( trimmed.StartsWith( wxS( "TOOL " ) ) )
        {
            wxString remainder = trimmed.Mid( 5 );
            remainder.Trim( true ).Trim( false );
            wxString toolName = remainder.BeforeFirst( ' ' ).Trim();
            wxString payload;

            if( remainder.Contains( wxS( " " ) ) )
                payload = remainder.AfterFirst( ' ' ).Trim( true ).Trim( false );

            queueToolCall( toolName, payload );
            continue;
        }

        if( !firstLine )
            filtered << wxS( "\n" );
        filtered << line;
        firstLine = false;
    }

    return filtered;
}


void SCH_OLLAMA_AGENT_PANE::appendThinkingText( const wxString& aText )
{
    if( aText.IsEmpty() )
        return;

    if( !m_reasoningBubble )
        m_reasoningBubble = addMessageToChat( wxEmptyString, CHAT_BUBBLE_KIND::THINKING );

    m_hasReasoningContent = true;
    m_reasoningText += aText;

    if( MESSAGE_BUBBLE* bubble = dynamic_cast<MESSAGE_BUBBLE*>( m_reasoningBubble ) )
    {
        wxString displayText = m_reasoningText;
        displayText.Trim( false );
        bubble->UpdateText( displayText );
        m_chatSizer->Layout();
        m_chatPanel->Layout();
    }

    scrollToBottom();
}


void SCH_OLLAMA_AGENT_PANE::appendAgentResponse( const wxString& aText )
{
    if( aText.IsEmpty() )
        return;

    m_streamingText += aText;
    m_streamBubbleDirty = true;

    if( !m_streamingBubble )
        m_streamingBubble = addMessageToChat( wxEmptyString, CHAT_BUBBLE_KIND::AGENT );

    const size_t IMMEDIATE_THRESHOLD = 256;

    if( aText.Length() > IMMEDIATE_THRESHOLD || aText.Find( '\n' ) != wxNOT_FOUND )
    {
        flushStreamBubble();
    }
    else if( !m_streamUpdateTimer.IsRunning() )
    {
        m_streamUpdateTimer.StartOnce( STREAM_UPDATE_INTERVAL_MS );
    }

    m_responseAccumulator += aText;
    scrollToBottom();
    m_chatPanel->Refresh();
}


void SCH_OLLAMA_AGENT_PANE::processStreamChunk( const wxString& aChunk )
{
    wxString chunk = filterToolLines( aChunk, true );

    if( chunk.IsEmpty() )
        return;

    const wxString THINK_START = wxS( "<think>" );
    const wxString THINK_END = wxS( "</think>" );
    wxString remaining = chunk;

    while( !remaining.IsEmpty() )
    {
        if( m_inThinkSection )
        {
            int endIndex = remaining.Find( THINK_END );
            if( endIndex == wxNOT_FOUND )
            {
                appendThinkingText( remaining );
                remaining.clear();
            }
            else
            {
                appendThinkingText( remaining.Left( endIndex ) );
                remaining = remaining.Mid( endIndex + THINK_END.length() );
                m_inThinkSection = false;
            }
        }
        else
        {
            int startIndex = remaining.Find( THINK_START );
            if( startIndex == wxNOT_FOUND )
            {
                appendAgentResponse( remaining );
                remaining.clear();
            }
            else
            {
                if( startIndex > 0 )
                    appendAgentResponse( remaining.Left( startIndex ) );

                remaining = remaining.Mid( startIndex + THINK_START.length() );
                m_inThinkSection = true;

                if( !m_reasoningBubble )
                    m_reasoningBubble = addMessageToChat( wxEmptyString, CHAT_BUBBLE_KIND::THINKING );
            }
        }
    }
}


void SCH_OLLAMA_AGENT_PANE::finalizeThinkingBubble()
{
    if( !m_reasoningBubble )
        return;

    if( !m_hasReasoningContent )
    {
        clearReasoningBubble();
        return;
    }

    if( MESSAGE_BUBBLE* bubble = dynamic_cast<MESSAGE_BUBBLE*>( m_reasoningBubble ) )
    {
        wxString text = m_reasoningText;
        text.Trim( true ).Trim( false );
        bubble->UpdateText( text );
    }

    m_reasoningBubble = nullptr;
    m_inThinkSection = false;
}

void SCH_OLLAMA_AGENT_PANE::flushStreamBubble()
{
    if( !m_streamBubbleDirty )
        return;

    m_streamBubbleDirty = false;

    if( MESSAGE_BUBBLE* bubble = dynamic_cast<MESSAGE_BUBBLE*>( m_streamingBubble ) )
    {
        bubble->UpdateText( m_streamingText );
        m_chatSizer->Layout();
        m_chatPanel->Layout();
    }
}


void SCH_OLLAMA_AGENT_PANE::OnStreamUpdateTimer( wxTimerEvent& aEvent )
{
    (void) aEvent;

    flushStreamBubble();
}


wxString SCH_OLLAMA_AGENT_PANE::sanitizeFinalResponse( const wxString& aResponse )
{
    wxString withoutTools = filterToolLines( aResponse, false );
    const wxString THINK_START = wxS( "<think>" );
    const wxString THINK_END = wxS( "</think>" );
    wxString cleaned;
    wxString remaining = withoutTools;

    while( !remaining.IsEmpty() )
    {
        int start = remaining.Find( THINK_START );
        if( start == wxNOT_FOUND )
        {
            cleaned += remaining;
            break;
        }

        cleaned += remaining.Left( start );
        remaining = remaining.Mid( start + THINK_START.length() );
        int end = remaining.Find( THINK_END );

        if( end == wxNOT_FOUND )
            break;

        remaining = remaining.Mid( end + THINK_END.length() );
    }

    return cleaned;
}


void SCH_OLLAMA_AGENT_PANE::OnRequestCancelled( wxCommandEvent& aEvent )
{
    (void)aEvent;
    clearReasoningBubble();

    if( m_streamingBubble )
        m_streamingBubble = nullptr;
    m_streamingText.clear();
    m_streamUpdateTimer.Stop();
    m_streamBubbleDirty = false;

    m_isProcessing = false;
    if( m_sendButton )
    {
        m_sendButton->Enable( true );
        m_sendButton->SetLabel( _( "Send" ) );
    }
    if( m_cancelButton )
        m_cancelButton->Enable( false );

    m_cancelRequested.store( false );

    m_toolCallQueue.clear();
    m_toolCallActive = false;

    AddAgentMessage( _( "Request cancelled. Ready when you are." ) );
    scrollToBottom();
}


void SCH_OLLAMA_AGENT_PANE::OnConnectionCheckResult( wxCommandEvent& aEvent )
{
    if( !m_statusText )
        return;

    bool success = ( aEvent.GetInt() == 1 );
    m_statusText->SetLabel( aEvent.GetString() );
    m_statusText->SetForegroundColour( success ? CURSOR_SUCCESS : CURSOR_DANGER );
    m_statusText->Refresh();
}


void SCH_OLLAMA_AGENT_PANE::onSendButton( wxCommandEvent& aEvent )
{
    sendMessage();
}






void SCH_OLLAMA_AGENT_PANE::sendMessage()
{
    wxString message = m_inputCtrl->GetText().Trim();
    
    if( message.IsEmpty() || m_isProcessing || !m_tool )
        return;
    
    m_cancelRequested.store( false );
    // Check if previous thread is still running
    {
        std::lock_guard<std::mutex> lock( m_threadMutex );
        if( m_requestThread.joinable() )
        {
            m_requestThread.join();
        }
    }
    
    // Add user message to chat
    AddUserMessage( message );
    m_streamingBubble = nullptr;
    m_streamingText.clear();
    m_responseAccumulator.clear();
    clearReasoningBubble();
    m_toolCallQueue.clear();
    m_toolCallActive = false;
    
    // Clear input
    m_inputCtrl->ClearAll();
    m_inputCtrl->SetFocus();
    
    m_isProcessing = true;
    if( m_sendButton )
    {
        m_sendButton->Enable( false );
        m_sendButton->SetLabel( _( "Processing..." ) );
    }
    if( m_cancelButton )
        m_cancelButton->Enable( true );

    if( m_statusText )
    {
        m_statusText->SetLabel( _( "Processing request..." ) );
        m_statusText->SetForegroundColour( CURSOR_MUTED );
    }

    // Show thinking indicator
    m_reasoningText.clear();
    m_hasReasoningContent = false;
    m_inThinkSection = false;
    m_reasoningBubble = addMessageToChat( wxEmptyString, CHAT_BUBBLE_KIND::THINKING );
    
    // Build prompt on main thread
    wxString prompt = m_tool->BuildPrompt( message );
    wxString model = m_tool->GetModel();
    const wxString& systemPrompt = m_tool->GetSystemPrompt();
    
    // Run Ollama request in background thread
    {
        std::lock_guard<std::mutex> lock( m_threadMutex );
        m_requestThread = std::thread( [this, prompt, model, systemPrompt]()
        {
                wxString response;
                bool success = false;
                wxString errorMessage;
                REQUEST_FAILURE_REASON failureReason = REQUEST_FAILURE_REASON::GENERIC;
                OLLAMA_CLIENT* client = ( m_tool ? m_tool->GetOllama() : nullptr );

                if( m_tool && client )
                {
                    if( !client->IsAvailable() )
                    {
                        errorMessage = _( "Error: Python agent not available. Make sure the agent is running (default: http://127.0.0.1:5001)" );
                        failureReason = REQUEST_FAILURE_REASON::AGENT_UNAVAILABLE;
                    }
                    else
                    {
                        auto chunkCallback = [this]( const wxString& chunk )
                        {
                            if( chunk.IsEmpty() || m_cancelRequested.load() )
                                return;

                            wxCommandEvent* event = new wxCommandEvent( wxEVT_COMMAND_TEXT_UPDATED, ID_RESPONSE_PARTIAL );
                            event->SetString( chunk );
                            wxQueueEvent( this, event );
                        };

                        success = client->StreamChatCompletion( model, prompt, chunkCallback,
                                                                response, &m_cancelRequested,
                                                                systemPrompt );
                    }
                }
                else
                {
                    errorMessage = _( "Error: Python agent not available. Make sure the agent is running (default: http://127.0.0.1:5001)" );
                    failureReason = REQUEST_FAILURE_REASON::AGENT_UNAVAILABLE;
                }

                if( m_cancelRequested.load() )
                {
                    wxCommandEvent* event = new wxCommandEvent( wxEVT_COMMAND_TEXT_UPDATED, ID_REQUEST_CANCELLED );
                    wxQueueEvent( this, event );
                    return;
                }

                if( !errorMessage.IsEmpty() )
                {
                    wxCommandEvent* event = new wxCommandEvent( wxEVT_COMMAND_TEXT_UPDATED, ID_REQUEST_FAILED );
                    event->SetString( errorMessage );
                    event->SetInt( static_cast<int>( failureReason ) );
                    wxQueueEvent( this, event );
                }
                else if( success )
                {
                    wxString responseCopy = response;
                    wxCommandEvent* event = new wxCommandEvent( wxEVT_COMMAND_TEXT_UPDATED, ID_RESPONSE_RECEIVED );
                    event->SetString( responseCopy );
                    wxQueueEvent( this, event );
                }
                else
                {
                    wxCommandEvent* event = new wxCommandEvent( wxEVT_COMMAND_TEXT_UPDATED, ID_REQUEST_FAILED );
                    event->SetInt( static_cast<int>( REQUEST_FAILURE_REASON::GENERIC ) );
                    wxQueueEvent( this, event );
                }
            } );
    }
    
    scrollToBottom();
}


void SCH_OLLAMA_AGENT_PANE::OnResponseReceived( wxCommandEvent& aEvent )
{
    wxString response = sanitizeFinalResponse( aEvent.GetString() );
    
    if( m_cancelRequested.load() )
        return;

    finalizeThinkingBubble();
    
    // Re-enable send button
    m_isProcessing = false;
    m_sendButton->Enable( true );
    m_sendButton->SetLabel( _( "Send" ) );
    if( m_cancelButton )
        m_cancelButton->Enable( false );
    m_cancelRequested.store( false );

    if( m_statusText )
    {
        m_statusText->SetLabel( _( "Connected to Python agent" ) );
        m_statusText->SetForegroundColour( CURSOR_SUCCESS );
    }
    
    // Ensure the full response is displayed in the chat
    m_streamUpdateTimer.Stop();
    flushStreamBubble();
    m_streamBubbleDirty = false;

    if( m_streamingBubble )
    {
        MESSAGE_BUBBLE* bubble = dynamic_cast<MESSAGE_BUBBLE*>( m_streamingBubble );
        if( bubble )
        {
            bubble->UpdateText( response );
            m_chatSizer->Layout();
            m_chatPanel->Layout();
        }
    }
    else
    {
        addMessageToChat( response, CHAT_BUBBLE_KIND::AGENT );
    }

    m_responseAccumulator = response;
    
    // Parse and execute commands from the response
    if( m_tool )
    {
        m_tool->ParseAndExecute( response );
    }
    
    m_streamingBubble = nullptr;
    m_streamingText.clear();
    
    scrollToBottom();
}


void SCH_OLLAMA_AGENT_PANE::OnResponsePartial( wxCommandEvent& aEvent )
{
    wxString chunk = aEvent.GetString();
    if( chunk.IsEmpty() )
        return;

    if( m_cancelRequested.load() )
        return;

    processStreamChunk( chunk );
}


void SCH_OLLAMA_AGENT_PANE::OnRequestFailed( wxCommandEvent& aEvent )
{
    if( m_cancelRequested.load() )
        return;

    wxString errorMessage = aEvent.GetString();
    if( errorMessage.IsEmpty() )
    {
        errorMessage = _( "Error: Failed to communicate with Python agent. Make sure the agent is running (default: http://127.0.0.1:5001)" );
    }

    REQUEST_FAILURE_REASON reason =
            static_cast<REQUEST_FAILURE_REASON>( aEvent.GetInt() );

    if( reason != REQUEST_FAILURE_REASON::AGENT_UNAVAILABLE )
        reason = REQUEST_FAILURE_REASON::GENERIC;

    // Remove thinking indicator
    clearReasoningBubble();
    
    if( m_streamingBubble )
        m_streamingBubble = nullptr;
    m_streamingText.clear();
    m_streamUpdateTimer.Stop();
    m_streamBubbleDirty = false;

    // Re-enable send button
    m_isProcessing = false;
    if( m_sendButton )
    {
        m_sendButton->Enable( true );
        m_sendButton->SetLabel( _( "Send" ) );
    }
    if( m_cancelButton )
        m_cancelButton->Enable( false );
    m_cancelRequested.store( false );
    if( m_statusText )
    {
        wxString statusText;

        if( reason == REQUEST_FAILURE_REASON::AGENT_UNAVAILABLE )
            statusText = _( "Unable to reach Python agent" );
        else
            statusText = _( "Agent request failed" );

        m_statusText->SetLabel( statusText );
        m_statusText->SetForegroundColour( CURSOR_DANGER );
    }
    
    AddAgentMessage( errorMessage );
    m_toolCallQueue.clear();
    m_toolCallActive = false;
    
    scrollToBottom();
}


void SCH_OLLAMA_AGENT_PANE::scrollToBottom()
{
    // Update virtual size first to ensure scrolling works correctly
    wxSize sizerSize = m_chatSizer->GetMinSize();
    int panelWidth = m_chatPanel->GetClientSize().GetWidth();
    if( panelWidth <= 0 )
        panelWidth = sizerSize.GetWidth();

    m_chatPanel->SetVirtualSize( wxSize( panelWidth, sizerSize.GetHeight() ) );
    
    // Scroll to bottom
    int scrollUnitY = 0;
    m_chatPanel->GetScrollPixelsPerUnit( nullptr, &scrollUnitY );
    if( scrollUnitY > 0 )
    {
        int maxY = ( sizerSize.GetHeight() + scrollUnitY - 1 ) / scrollUnitY;
        m_chatPanel->Scroll( 0, maxY );
    }
    else
    {
        // Fallback: scroll to maximum
        wxSize virtualSize = m_chatPanel->GetVirtualSize();
        m_chatPanel->Scroll( 0, virtualSize.GetHeight() );
    }
    
    m_chatPanel->Refresh();
}
