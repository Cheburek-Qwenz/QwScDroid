package com.compose.sctool

import android.content.Intent
import android.net.Uri
import androidx.compose.animation.*
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.pager.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch

data class OnboardPage(
    val titleRu: String,
    val titleEn: String,
    val descRu: String,
    val descEn: String,
    val icon: androidx.compose.ui.graphics.vector.ImageVector
)

val ONBOARD_PAGES = listOf(
    OnboardPage(
        "Добро пожаловать!", "Welcome!",
        "QwScDroid- инструментарий для моддинга Brawl Stars прямо на телефоне.Без ПК.",
        "QwScDroid — a Brawl Stars modding toolkit right on your phone. No PC needed.",
        Icons.Rounded.Smartphone
    ),
    OnboardPage(
        "Конвертация текстур", "Texture Conversion",
        "Конвертируй SCTX,KTX,PNG и прочие форматы",
        "Convert SCTX, KTX and SC textures to PNG and back. ASTC encoding supported.",
        Icons.Rounded.Image
    ),
    OnboardPage(
        "Работа с SC файлами", "SC File Tools",
        "Разбивай, склеивай, даунгрейди SC файлы. Извлекай KTX текстуры из SC.",
        "Split, join, downgrade SC files. Extract KTX textures from SC archives.",
        Icons.Rounded.FolderOpen
    ),
    OnboardPage(
        "Программа в разработке.", "Beta Version",
        "Это бета. Возможны баги.\n\nНашёл проблему? Пиши:\nTG: @asdfghjkl12345678\nили в @Cheburek_dev,ну а так все,приятного испрльзования!",
        "This is beta. Bugs may occur.\n\nFound an issue? Contact:\nTG: @asdfghjkl12345678\nor @Cheburek_dev",
        Icons.Rounded.BugReport
    ),
)

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun WelcomeScreen(isRu: Boolean, onDone: () -> Unit) {
    val pagerState = rememberPagerState { ONBOARD_PAGES.size }
    val scope      = rememberCoroutineScope()
    val ctx        = LocalContext.current
    val isLast     = pagerState.currentPage == ONBOARD_PAGES.lastIndex

    Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)) {
        HorizontalPager(state = pagerState, modifier = Modifier.fillMaxSize()) { idx ->
            val page = ONBOARD_PAGES[idx]
            Column(
                Modifier.fillMaxSize().padding(32.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                // Icon circle
                Surface(
                    shape = CircleShape,
                    color = MaterialTheme.colorScheme.primaryContainer,
                    modifier = Modifier.size(96.dp)
                ) {
                    Box(contentAlignment = Alignment.Center) {
                        Icon(page.icon, null, Modifier.size(48.dp),
                            tint = MaterialTheme.colorScheme.onPrimaryContainer)
                    }
                }
                Spacer(Modifier.height(32.dp))
                Text(
                    if (isRu) page.titleRu else page.titleEn,
                    style = MaterialTheme.typography.headlineMedium,
                    fontWeight = FontWeight.Bold,
                    textAlign = TextAlign.Center,
                    color = MaterialTheme.colorScheme.onBackground
                )
                Spacer(Modifier.height(16.dp))
                Text(
                    if (isRu) page.descRu else page.descEn,
                    style = MaterialTheme.typography.bodyLarge,
                    textAlign = TextAlign.Center,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    lineHeight = 24.sp
                )
                // Ссылка на TG на последней странице
                if (idx == ONBOARD_PAGES.lastIndex) {
                    Spacer(Modifier.height(16.dp))
                    OutlinedButton(
                        onClick = {
                            ctx.startActivity(Intent(Intent.ACTION_VIEW,
                                Uri.parse("https://t.me/Cheburek_dev")))
                        },
                        shape = RoundedCornerShape(12.dp)
                    ) {
                        Icon(Icons.Rounded.Send, null, Modifier.size(16.dp))
                        Spacer(Modifier.width(8.dp))
                        Text("@Cheburek_dev")
                    }
                }
            }
        }

        // Bottom controls
        Column(
            Modifier.align(Alignment.BottomCenter).padding(32.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // Dots
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                repeat(ONBOARD_PAGES.size) { idx ->
                    val selected = pagerState.currentPage == idx
                    AnimatedContent(targetState = selected) { sel ->
                        Box(
                            Modifier
                                .clip(CircleShape)
                                .background(
                                    if (sel) MaterialTheme.colorScheme.primary
                                    else MaterialTheme.colorScheme.outlineVariant
                                )
                                .size(if (sel) 10.dp else 8.dp)
                        )
                    }
                }
            }
            Spacer(Modifier.height(24.dp))

            // Next / Done button
            Button(
                onClick = {
                    if (isLast) onDone()
                    else scope.launch { pagerState.animateScrollToPage(pagerState.currentPage + 1) }
                },
                modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(14.dp)
            ) {
                Text(
                    when {
                        isLast && isRu -> "Начать!"
                        isLast         -> "Get started!"
                        isRu           -> "Далее"
                        else           -> "Next"
                    },
                    fontSize = 16.sp,
                    fontWeight = FontWeight.SemiBold
                )
            }

            // Skip
            if (!isLast) {
                TextButton(onClick = onDone) {
                    Text(if (isRu) "Пропустить" else "Skip",
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
    }
}
